#ifndef XRECOMP_HOST_TIME_H
#define XRECOMP_HOST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t xrecomp_host_monotonic_ms(void);
uint64_t xrecomp_host_monotonic_ns(void);
void xrecomp_host_sleep_ms(uint32_t milliseconds);
uint64_t xrecomp_host_thread_id(void);

#ifdef __cplusplus
}
#endif

#endif /* XRECOMP_HOST_TIME_H */
