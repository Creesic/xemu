#ifndef XRECOMP_PLATFORM_TRACY_H
#define XRECOMP_PLATFORM_TRACY_H

/*
 * Shared, title-neutral Tracy facade.
 *
 * Keep runtime sources independent of Tracy's build option and make disabled
 * builds true compile-time no-ops. C callers use explicit begin/end pairs;
 * C++ callers may use the RAII scoped form for early-return-safe functions.
 */
#if defined(XRECOMP_TRACY)
#include <tracy/TracyC.h>
#ifdef __cplusplus
#include <tracy/Tracy.hpp>
#endif

#define XRECOMP_TRACY_THREAD_NAME(name) TracyCSetThreadName(name)
#define XRECOMP_TRACY_FIBER_ENTER(name) \
    do { TracyCFiberEnter(name) } while (0)
#define XRECOMP_TRACY_FIBER_LEAVE() \
    do { TracyCFiberLeave } while (0)
#define XRECOMP_TRACY_ZONE_BEGIN(ctx, name) TracyCZoneN(ctx, name, 1)
#define XRECOMP_TRACY_ZONE_END(ctx) TracyCZoneEnd(ctx)
#define XRECOMP_TRACY_ZONE_VALUE(ctx, value) \
    TracyCZoneValue(ctx, (uint64_t)(value))
#define XRECOMP_TRACY_ZONE_TEXT(ctx, text, size) \
    TracyCZoneText(ctx, text, size)
#define XRECOMP_TRACY_FRAME_MARK(name) TracyCFrameMarkNamed(name)
#define XRECOMP_TRACY_PLOT_I(name, value) \
    TracyCPlotI(name, (int64_t)(value))
#ifdef __cplusplus
#define XRECOMP_TRACY_ZONE_SCOPED(name) ZoneScopedN(name)
#endif

#else

#define XRECOMP_TRACY_THREAD_NAME(name) ((void)0)
#define XRECOMP_TRACY_FIBER_ENTER(name) ((void)0)
#define XRECOMP_TRACY_FIBER_LEAVE() ((void)0)
#define XRECOMP_TRACY_ZONE_BEGIN(ctx, name) ((void)0)
#define XRECOMP_TRACY_ZONE_END(ctx) ((void)0)
#define XRECOMP_TRACY_ZONE_VALUE(ctx, value) ((void)0)
#define XRECOMP_TRACY_ZONE_TEXT(ctx, text, size) ((void)0)
#define XRECOMP_TRACY_FRAME_MARK(name) ((void)0)
#define XRECOMP_TRACY_PLOT_I(name, value) ((void)0)
#ifdef __cplusplus
#define XRECOMP_TRACY_ZONE_SCOPED(name) ((void)0)
#endif

#endif

#if defined(XRECOMP_TRACY) && defined(XRECOMP_TRACY_GUEST_FUNCTIONS)
#define XRECOMP_TRACY_GUEST_ZONE_BEGIN(ctx) \
    XRECOMP_TRACY_ZONE_BEGIN(ctx, __func__)
#define XRECOMP_TRACY_GUEST_ZONE_END(ctx) XRECOMP_TRACY_ZONE_END(ctx)
#else
#define XRECOMP_TRACY_GUEST_ZONE_BEGIN(ctx) ((void)0)
#define XRECOMP_TRACY_GUEST_ZONE_END(ctx) ((void)0)
#endif

#endif
