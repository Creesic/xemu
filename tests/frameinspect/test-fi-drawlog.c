#include <stdio.h>
#include "../../xemu-frameinspect-drawlog.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

static int test_log_and_aggregation(void)
{
    uint64_t initial_bytes = fi_drawlog_initial_bytes();
    FIBudget budget = { .limit = initial_bytes * 4, .used = 0 };
    FIDrawLog log;
    CHECK(fi_drawlog_init(&log, &budget));
    CHECK(budget.used == initial_bytes);
    CHECK(log.open_submission == FI_DRAW_INVALID);

    uint32_t draw = fi_drawlog_begin(&log, &budget, 42, 0,
                                     FI_DRAW_ROUTE_ARRAYS, 5);
    CHECK(draw == 0);
    CHECK(log.submissions[draw].batch_event == 42);
    CHECK(log.submissions[draw].attrs[15].slot == 15);
    CHECK(log.submissions[draw].attrs[15].status == FI_VALUE_UNAVAILABLE);
    CHECK(log.submissions[draw].textures[3].resource_id == FI_DRAW_INVALID);
    CHECK(fi_drawlog_begin(&log, &budget, 42, 1, FI_DRAW_ROUTE_ARRAYS, 5) ==
          FI_DRAW_INVALID);

    FIDrawSegment segments[] = {
        { .first = 3, .count = 6 },
        { .first = 20, .count = 9 },
    };
    CHECK(fi_drawlog_append_segments(&log, &budget, draw, segments, 2));
    CHECK(log.segments[0].first == 3 && log.segments[1].first == 20);
    CHECK(!fi_drawlog_append_segments(&log, &budget, draw, segments, 2));

    for (uint32_t i = 0; i < FI_DRAW_SAMPLE_LIMIT; i++) {
        FIVertexSample sample = { .ordinal = i, .source_index = 20 + i };
        sample.attrs[3].status = FI_VALUE_PRESENT;
        sample.attrs[3].raw_len = 4;
        sample.attrs[3].raw[0] = i;
        sample.attrs[3].decoded[0] = (float)i;
        CHECK(fi_drawlog_append_sample(&log, &budget, draw, &sample) == i);
    }
    FIVertexSample excess = { .ordinal = 8, .source_index = 28 };
    CHECK(fi_drawlog_append_sample(&log, &budget, draw, &excess) ==
          FI_DRAW_INVALID);
    CHECK(log.submissions[draw].flags & FI_DRAW_SAMPLE_TRUNCATED);

    FIStateSource source = {
        .destination = { .kind = 2, .index = 7, .component = 1 },
        .source_token = 19,
        .phys_addr = 0x03f00040,
        .command_id = 77,
        .method = 0x1e20,
        .parameter = 0x11223344,
        .writer_node = 51,
        .confidence = FI_ORIG_ATTRIBUTED,
    };
    CHECK(fi_drawlog_append_source(&log, &budget, draw, &source) == 0);
    source.parameter = 0;
    CHECK(log.sources[0].parameter == 0x11223344);

    FIWriterSpan spans[23] = {
        { .writer_node = 7, .confidence = FI_ORIG_ATTRIBUTED, .bytes = 5 },
        { .writer_node = 3, .confidence = FI_ORIG_PARTIAL, .bytes = 20 },
        { .writer_node = 7, .confidence = FI_ORIG_ATTRIBUTED, .bytes = 7 },
        { .writer_node = 0, .confidence = FI_ORIG_UNATTRIBUTED, .bytes = 9 },
    };
    for (uint32_t i = 4; i < 23; i++) {
        spans[i].writer_node = 100 + i;
        spans[i].confidence = FI_ORIG_ATTRIBUTED;
        spans[i].bytes = 1;
    }
    uint32_t writer_set =
        fi_drawlog_append_writer_set(&log, &budget, draw, spans, 23);
    CHECK(writer_set == 0);
    const FIResourceWriterSet *set = &log.writer_sets[writer_set];
    CHECK(set->coverage.total_bytes == 60);
    CHECK(set->coverage.attributed_bytes == 31);
    CHECK(set->coverage.partial_bytes == 20);
    CHECK(set->coverage.unattributed_bytes == 9);
    CHECK(set->coverage.omitted_bytes == 6);
    CHECK(set->coverage.truncated_bytes == 0);
    CHECK(set->num_writers == FI_DRAW_DOMINANT_WRITER_LIMIT);
    CHECK(log.writers[set->first_writer].writer_node == 3);
    CHECK(log.writers[set->first_writer].bytes == 20);
    CHECK(log.writers[set->first_writer + 1].writer_node == 7);
    CHECK(log.writers[set->first_writer + 1].bytes == 12);

    FIWriterSpan *many_spans = (FIWriterSpan *)calloc(
        FI_DRAW_WRITER_CANDIDATE_LIMIT + 1, sizeof(FIWriterSpan));
    CHECK(many_spans != NULL);
    for (uint32_t i = 0; i <= FI_DRAW_WRITER_CANDIDATE_LIMIT; i++) {
        many_spans[i].writer_node = 1000 + i;
        many_spans[i].confidence = FI_ORIG_ATTRIBUTED;
        many_spans[i].bytes = 1;
    }
    FIResourceWriter dominant[FI_DRAW_DOMINANT_WRITER_LIMIT];
    FIWriterCoverage capped = fi_writer_aggregate(
        many_spans, FI_DRAW_WRITER_CANDIDATE_LIMIT + 1, dominant);
    CHECK(capped.total_bytes == FI_DRAW_WRITER_CANDIDATE_LIMIT + 1);
    CHECK(capped.dominant_count == FI_DRAW_DOMINANT_WRITER_LIMIT);
    CHECK(capped.truncated_bytes == 1);
    CHECK(dominant[0].writer_node == 1000);
    free(many_spans);

    CHECK(fi_drawlog_complete(&log, draw));
    CHECK(log.submissions[draw].flags & FI_DRAW_COMPLETE);
    CHECK(fi_drawlog_append_source(&log, &budget, draw, &source) ==
          FI_DRAW_INVALID);

    uint32_t indexed = fi_drawlog_begin(&log, &budget, 43, 0,
                                        FI_DRAW_ROUTE_INDEXED, 4);
    CHECK(indexed == 1);
    uint32_t indices[] = { 4, 2, 4, 9 };
    CHECK(fi_drawlog_append_indices(&log, &budget, indexed, indices, 4));
    CHECK(log.indices[log.submissions[indexed].first_index + 2] == 4);
    CHECK(fi_drawlog_complete(&log, indexed));

    uint32_t aborted = fi_drawlog_begin(&log, &budget, 44, 0,
                                        FI_DRAW_ROUTE_INLINE_ARRAY, 5);
    CHECK(aborted == 2);
    FIVertexSample sample = { .ordinal = 0, .source_index = 0 };
    CHECK(fi_drawlog_append_sample(&log, &budget, aborted, &sample) !=
          FI_DRAW_INVALID);
    uint32_t old_samples = log.submissions[aborted].first_sample;
    uint32_t old_writer_sets = log.submissions[aborted].first_writer_set;
    uint32_t old_writers = log.submissions[aborted].first_writer;
    FIWriterSpan aborted_span = {
        .writer_node = 91,
        .confidence = FI_ORIG_ATTRIBUTED,
        .bytes = 4,
    };
    CHECK(fi_drawlog_append_writer_set(
        &log, &budget, aborted, &aborted_span, 1) != FI_DRAW_INVALID);
    CHECK(fi_drawlog_abort(&log, aborted));
    CHECK(log.num_submissions == 2);
    CHECK(log.num_samples == old_samples);
    CHECK(log.num_writer_sets == old_writer_sets);
    CHECK(log.num_writers == old_writers);
    CHECK(!log.truncated);

    fi_drawlog_free(&log, &budget);
    CHECK(budget.used == 0);
    CHECK(log.submissions == NULL);
    CHECK(log.open_submission == FI_DRAW_INVALID);
    return 0;
}

static int test_budget_rollback(void)
{
    uint64_t initial_bytes = fi_drawlog_initial_bytes();
    FIBudget too_small = { .limit = initial_bytes - 1, .used = 0 };
    FIDrawLog failed;
    CHECK(!fi_drawlog_init(&failed, &too_small));
    CHECK(too_small.used == 0);

    FIBudget budget = { .limit = initial_bytes, .used = 0 };
    FIDrawLog log;
    CHECK(fi_drawlog_init(&log, &budget));
    CHECK(budget.used == initial_bytes);
    uint32_t draw = fi_drawlog_begin(&log, &budget, 1, 0,
                                     FI_DRAW_ROUTE_ARRAYS, 5);
    CHECK(draw == 0);

    FIDrawSegment *segments = (FIDrawSegment *)calloc(
        FI_DRAWLOG_SEGMENT_INITIAL_CAP + 1, sizeof(FIDrawSegment));
    CHECK(segments != NULL);
    uint32_t before = log.num_segments;
    CHECK(!fi_drawlog_append_segments(
        &log, &budget, draw, segments, FI_DRAWLOG_SEGMENT_INITIAL_CAP + 1));
    CHECK(log.num_segments == before);
    CHECK(log.submissions[draw].num_segments == 0);
    CHECK(budget.used == initial_bytes);
    CHECK(log.truncated);
    free(segments);

    CHECK(fi_drawlog_abort(&log, draw));
    fi_drawlog_free(&log, &budget);
    CHECK(budget.used == 0);
    return 0;
}

int main(void)
{
    CHECK(test_log_and_aggregation() == 0);
    CHECK(test_budget_rollback() == 0);
    printf("PASS\n");
    return 0;
}
