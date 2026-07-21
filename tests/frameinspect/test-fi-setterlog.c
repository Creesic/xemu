#include <stdio.h>
#include "../../xemu-frameinspect-setterlog.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

static FISetterDestination destination(uint16_t kind, uint16_t index,
                                       uint16_t component, uint16_t field)
{
    return (FISetterDestination) {
        .kind = kind,
        .index = index,
        .component = component,
        .reserved = field,
    };
}

static int test_latest_and_snapshot(void)
{
    uint64_t initial_bytes = fi_setterlog_initial_bytes();
    FIBudget budget = { .limit = initial_bytes * 4, .used = 0 };
    FISetterLog log;
    CHECK(fi_setterlog_init(&log, &budget));
    CHECK(budget.used == initial_bytes);
    CHECK(log.next_token == 1);

    uint64_t first = fi_setterlog_begin_source(
        &log, &budget, 0x1e20, 0, 0x11223344, 0x03f00040, 51,
        FI_ORIG_ATTRIBUTED);
    CHECK(first == 1);
    CHECK(log.num_sources == 1);
    CHECK(log.sources[0].command_id == FI_SETTER_COMMAND_INVALID);

    FISetterDestination reg = destination(
        FI_SETTER_DEST_PGRAPH_REGISTER, 0x320, 0, 0);
    FISetterDestination constant = destination(
        FI_SETTER_DEST_VSH_CONSTANT, 17, 2, 0);
    CHECK(fi_setterlog_record_destination(&log, &budget, first, &reg));
    CHECK(fi_setterlog_record_destination(&log, &budget, first, &constant));
    CHECK(log.num_sources == 1);
    CHECK(log.num_destinations == 2);
    CHECK(fi_setterlog_lookup(&log, &reg)->token == first);
    CHECK(fi_setterlog_bind_command(&log, first, 77));
    CHECK(fi_setterlog_lookup(&log, &constant)->command_id == 77);

    uint64_t replacement = fi_setterlog_begin_source(
        &log, &budget, 0x1e20, 0, 0xaabbccdd, 0x03f00044, 63,
        FI_ORIG_PARTIAL);
    CHECK(replacement == 2);
    CHECK(fi_setterlog_record_destination(
        &log, &budget, replacement, &reg));
    CHECK(log.num_destinations == 2);
    CHECK(fi_setterlog_lookup(&log, &reg)->token == replacement);
    CHECK(fi_setterlog_lookup(&log, &constant)->token == first);
    CHECK(!fi_setterlog_record_destination(&log, &budget, 99, &reg));
    CHECK(!fi_setterlog_bind_command(&log, 99, 1));

    FISetterDestination missing = destination(
        FI_SETTER_DEST_TEXTURE, 2, 0, 5);
    FISetterDestination requested[] = { reg, constant, missing, reg };
    FISetterSnapshot snapshot = { 0 };
    CHECK(fi_setterlog_snapshot(&log, requested, 4, &snapshot, &budget));
    CHECK(snapshot.num_bindings == 4);
    CHECK(snapshot.num_sources == 2);
    CHECK(snapshot.bindings[0].status == FI_SETTER_BINDING_AVAILABLE);
    CHECK(snapshot.bindings[1].status == FI_SETTER_BINDING_AVAILABLE);
    CHECK(snapshot.bindings[2].status == FI_SETTER_BINDING_INHERITED);
    CHECK(snapshot.bindings[2].source_index == FI_SETTER_SOURCE_INVALID);
    CHECK(snapshot.bindings[0].source_index ==
          snapshot.bindings[3].source_index);
    uint32_t replacement_snapshot = snapshot.bindings[0].source_index;
    CHECK(snapshot.sources[replacement_snapshot].parameter == 0xaabbccdd);
    CHECK(snapshot.sources[replacement_snapshot].command_id ==
          FI_SETTER_COMMAND_INVALID);

    CHECK(fi_setterlog_bind_command(&log, replacement, 88));
    CHECK(snapshot.sources[replacement_snapshot].command_id ==
          FI_SETTER_COMMAND_INVALID);
    fi_setter_snapshot_free(&snapshot, &budget);
    CHECK(snapshot.sources == NULL);

    FIBudget snapshot_budget = { .limit = 1, .used = 0 };
    FISetterSnapshot failed_snapshot = { 0 };
    CHECK(!fi_setterlog_snapshot(
        &log, requested, 4, &failed_snapshot, &snapshot_budget));
    CHECK(failed_snapshot.sources == NULL);
    CHECK(failed_snapshot.bindings == NULL);
    CHECK(failed_snapshot.truncated);
    CHECK(snapshot_budget.used == 0);

    log.next_token = UINT64_MAX;
    uint32_t old_sources = log.num_sources;
    CHECK(fi_setterlog_begin_source(
              &log, &budget, 0, 0, 0, 0, 0, FI_ORIG_UNATTRIBUTED) ==
          FI_SETTER_TOKEN_INVALID);
    CHECK(log.num_sources == old_sources);
    CHECK(log.truncated);

    fi_setterlog_free(&log, &budget);
    CHECK(budget.used == 0);
    CHECK(log.sources == NULL);
    CHECK(log.next_token == 1);
    return 0;
}

static int test_source_budget_rollback(void)
{
    uint64_t initial_bytes = fi_setterlog_initial_bytes();
    FIBudget too_small = { .limit = initial_bytes - 1, .used = 0 };
    FISetterLog failed;
    CHECK(!fi_setterlog_init(&failed, &too_small));
    CHECK(too_small.used == 0);

    FIBudget budget = { .limit = initial_bytes, .used = 0 };
    FISetterLog log;
    CHECK(fi_setterlog_init(&log, &budget));
    for (uint32_t i = 0; i < FI_SETTER_SOURCE_INITIAL_CAP; i++) {
        CHECK(fi_setterlog_begin_source(
                  &log, &budget, i, 0, i, i * 4, i,
                  FI_ORIG_ATTRIBUTED) != FI_SETTER_TOKEN_INVALID);
    }
    uint32_t old_sources = log.num_sources;
    CHECK(fi_setterlog_begin_source(
              &log, &budget, 0, 0, 0, 0, 0, FI_ORIG_ATTRIBUTED) ==
          FI_SETTER_TOKEN_INVALID);
    CHECK(log.num_sources == old_sources);
    CHECK(budget.used == initial_bytes);
    CHECK(log.truncated);
    fi_setterlog_free(&log, &budget);
    CHECK(budget.used == 0);
    return 0;
}

static int test_destination_budget_rollback(void)
{
    uint64_t initial_bytes = fi_setterlog_initial_bytes();
    FIBudget budget = { .limit = initial_bytes, .used = 0 };
    FISetterLog log;
    CHECK(fi_setterlog_init(&log, &budget));
    uint64_t source = fi_setterlog_begin_source(
        &log, &budget, 1, 0, 2, 4, 3, FI_ORIG_ATTRIBUTED);
    CHECK(source != FI_SETTER_TOKEN_INVALID);

    uint32_t initial_entries = FI_SETTER_DESTINATION_INITIAL_CAP / 2;
    for (uint32_t i = 0; i < initial_entries; i++) {
        FISetterDestination dest = destination(
            FI_SETTER_DEST_PGRAPH_REGISTER, i, 0, 0);
        CHECK(fi_setterlog_record_destination(
            &log, &budget, source, &dest));
    }
    uint32_t old_destinations = log.num_destinations;
    FISetterDestination growth = destination(
        FI_SETTER_DEST_PGRAPH_REGISTER, initial_entries, 0, 0);
    CHECK(!fi_setterlog_record_destination(
        &log, &budget, source, &growth));
    CHECK(log.num_destinations == old_destinations);
    CHECK(fi_setterlog_lookup(&log, &growth) == NULL);
    CHECK(budget.used == initial_bytes);
    CHECK(log.truncated);
    fi_setterlog_free(&log, &budget);
    CHECK(budget.used == 0);
    return 0;
}

int main(void)
{
    CHECK(test_latest_and_snapshot() == 0);
    CHECK(test_source_budget_rollback() == 0);
    CHECK(test_destination_budget_rollback() == 0);
    printf("PASS\n");
    return 0;
}
