#ifndef XRECOMP_CPU_RECORDER_H
#define XRECOMP_CPU_RECORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum xrecomp_cpu_recorder_event_kind {
    XRECOMP_CPU_EVENT_GUEST_ENTER = 1,
    XRECOMP_CPU_EVENT_GUEST_EXIT = 2,
    XRECOMP_CPU_EVENT_RUNTIME_ENTER = 3,
    XRECOMP_CPU_EVENT_RUNTIME_EXIT = 4,
    XRECOMP_CPU_EVENT_FRAME_BEGIN = 5,
    XRECOMP_CPU_EVENT_FRAME_END = 6
};

/*
 * Hot-path gate read by generated code. Instrumented builds pay one predictable
 * load/branch while the recorder is idle; ordinary builds compile the guest
 * macros below to true no-ops.
 */
extern volatile uint32_t g_xrecomp_cpu_recorder_active;

/* Allocate and pre-fault the RAM arena. Safe to call in non-recorder builds. */
int xrecomp_cpu_recorder_initialize(const char *xbe_sha256);

/* Arm a bounded capture. Recording begins at the next accepted host present. */
int xrecomp_cpu_recorder_request(uint32_t frame_count);

/* Mark an accepted host-present boundary and advance the capture state. */
void xrecomp_cpu_recorder_present_boundary(void);

/* Preserve separate nesting/chunks across cooperative guest-fiber switches. */
void xrecomp_cpu_recorder_set_guest_thread(uint32_t guest_thread_handle);

/* Complete a partial capture, wait for any writer, and release the RAM arena. */
void xrecomp_cpu_recorder_shutdown(void);

/* Event entrypoints used by generated code and title-neutral runtime scopes. */
void xrecomp_cpu_recorder_guest_event(uint32_t kind, uint32_t guest_va);
uint32_t xrecomp_cpu_recorder_runtime_begin(const char *name);
void xrecomp_cpu_recorder_runtime_end(uint32_t name_id);

#ifdef __cplusplus
}
#endif

#if defined(XRECOMP_CPU_RECORDER)

#define XRECOMP_CPU_RECORDER_GUEST_ENTER(guest_va) do { \
    if (g_xrecomp_cpu_recorder_active) \
        xrecomp_cpu_recorder_guest_event( \
            XRECOMP_CPU_EVENT_GUEST_ENTER, (uint32_t)(guest_va)); \
} while (0)

#define XRECOMP_CPU_RECORDER_GUEST_EXIT(guest_va) do { \
    if (g_xrecomp_cpu_recorder_active) \
        xrecomp_cpu_recorder_guest_event( \
            XRECOMP_CPU_EVENT_GUEST_EXIT, (uint32_t)(guest_va)); \
} while (0)

#define XRECOMP_CPU_RECORDER_ZONE_BEGIN(ctx, name) \
    uint32_t ctx = g_xrecomp_cpu_recorder_active \
        ? xrecomp_cpu_recorder_runtime_begin(name) : 0u

#define XRECOMP_CPU_RECORDER_ZONE_END(ctx) do { \
    if ((ctx) != 0u) xrecomp_cpu_recorder_runtime_end(ctx); \
} while (0)

#ifdef __cplusplus
class XrecompCpuRecorderScope {
public:
    explicit XrecompCpuRecorderScope(const char *name)
        : id_(g_xrecomp_cpu_recorder_active
              ? xrecomp_cpu_recorder_runtime_begin(name) : 0u)
    {
    }

    ~XrecompCpuRecorderScope()
    {
        if (id_ != 0u)
            xrecomp_cpu_recorder_runtime_end(id_);
    }

    XrecompCpuRecorderScope(const XrecompCpuRecorderScope &) = delete;
    XrecompCpuRecorderScope &operator=(
        const XrecompCpuRecorderScope &) = delete;

private:
    uint32_t id_;
};

#define XRECOMP_CPU_RECORDER_CONCAT_INNER(a, b) a##b
#define XRECOMP_CPU_RECORDER_CONCAT(a, b) \
    XRECOMP_CPU_RECORDER_CONCAT_INNER(a, b)
#define XRECOMP_CPU_RECORDER_ZONE_SCOPED(name) \
    XrecompCpuRecorderScope XRECOMP_CPU_RECORDER_CONCAT( \
        _xrecomp_cpu_recorder_scope_, __LINE__)(name)
#endif

#else

#define XRECOMP_CPU_RECORDER_GUEST_ENTER(guest_va) ((void)0)
#define XRECOMP_CPU_RECORDER_GUEST_EXIT(guest_va) ((void)0)
#define XRECOMP_CPU_RECORDER_ZONE_BEGIN(ctx, name) ((void)0)
#define XRECOMP_CPU_RECORDER_ZONE_END(ctx) ((void)0)
#ifdef __cplusplus
#define XRECOMP_CPU_RECORDER_ZONE_SCOPED(name) ((void)0)
#endif

#endif

#endif /* XRECOMP_CPU_RECORDER_H */
