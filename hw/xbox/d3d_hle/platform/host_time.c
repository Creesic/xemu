#include "host_time.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

uint64_t xrecomp_host_monotonic_ms(void)
{
    return GetTickCount64();
}

uint64_t xrecomp_host_monotonic_ns(void)
{
    /* The performance-counter frequency is fixed at boot, so query it once.
     * Re-querying per timestamp doubled the API calls on a path that the perf
     * telemetry hits twice per dispatched NV2A method — over 100k calls a frame
     * on a busy one, which inflated the very measurement it was taking. */
    static uint64_t frequency;
    LARGE_INTEGER counter;

    if (!frequency) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency = (uint64_t)f.QuadPart;
        if (!frequency)
            frequency = 1;
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter.QuadPart / frequency * 1000000000ull +
           (uint64_t)counter.QuadPart % frequency * 1000000000ull / frequency;
}

void xrecomp_host_sleep_ms(uint32_t milliseconds)
{
    Sleep(milliseconds);
}

uint64_t xrecomp_host_thread_id(void)
{
    return GetCurrentThreadId();
}

#else

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

uint64_t xrecomp_host_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

uint64_t xrecomp_host_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

void xrecomp_host_sleep_ms(uint32_t milliseconds)
{
    struct timespec duration;
    duration.tv_sec = (time_t)(milliseconds / 1000u);
    duration.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}

uint64_t xrecomp_host_thread_id(void)
{
    pthread_t thread = pthread_self();
    const unsigned char *bytes = (const unsigned char *)&thread;
    uint64_t hash = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < sizeof(thread); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1u;
}

#endif
