#ifndef XRECOMP_TIMING_TRACE_H
#define XRECOMP_TIMING_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum xrecomp_timing_trace_kind {
    XRECOMP_TIMING_TRACE_BEGIN = 1,
    XRECOMP_TIMING_TRACE_END = 2,
    XRECOMP_TIMING_TRACE_INSTANT = 3,
    XRECOMP_TIMING_TRACE_DATA_TOUCH = 4,
    XRECOMP_TIMING_TRACE_GATE_ACCEPTED = 5,
    XRECOMP_TIMING_TRACE_GATE_SKIPPED = 6
};

#define XRECOMP_TIMING_EVENT_VBLANK          UINT32_C(0x80000001)
#define XRECOMP_TIMING_EVENT_PRESENT_ISSUED  UINT32_C(0x80000002)
#define XRECOMP_TIMING_EVENT_PRESENT_DEFERRED UINT32_C(0x80000003)
#define XRECOMP_TIMING_EVENT_PRESENT_SKIPPED UINT32_C(0x80000004)

/*
 * Fixed-layout on-disk record. Keep this in sync with
 * tools/timing/mm3_timing_analyze.py.
 */
typedef struct xrecomp_timing_trace_record {
    uint64_t sequence;
    uint64_t host_ns;
    uint32_t event_id;
    uint32_t guest_va;
    uint32_t kind;
    uint32_t thread_id;
    uint32_t regs[7];   /* eax, ebx, ecx, edx, esi, edi, esp */
    uint32_t stack[4];  /* entry stack words, including dummy return */
    uint32_t values[4]; /* event-specific values */
    uint32_t reserved;
} xrecomp_timing_trace_record;

/*
 * Start explicitly, primarily for tests. Production callers normally rely on
 * lazy initialization from XRECOMP_TIMING_TRACE. A value of "1" writes
 * xrecomp_timing_trace.bin; any other non-empty value is used as the path.
 *
 * The bounded ring has one producer. Current MM3 guest, vblank, and present
 * hooks all execute on the cooperative guest thread; new callers from another
 * host thread must serialize externally or replace this with an MPSC queue.
 */
int xrecomp_timing_trace_start(const char *path, uint32_t capacity_records);
int xrecomp_timing_trace_enabled(void);
void xrecomp_timing_trace_emit(
    uint32_t event_id,
    uint32_t guest_va,
    uint32_t kind,
    const uint32_t regs[7],
    const uint32_t stack[4],
    const uint32_t values[4]);
void xrecomp_timing_trace_data_touch(
    uint32_t guest_pc,
    uint32_t data_va,
    uint32_t value);
void xrecomp_timing_trace_shutdown(void);
uint64_t xrecomp_timing_trace_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* XRECOMP_TIMING_TRACE_H */
