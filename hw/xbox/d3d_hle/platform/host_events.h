#ifndef XRECOMP_HOST_EVENTS_H
#define XRECOMP_HOST_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Services pending host-window events without blocking. Returns the number
 * of messages removed from the current thread's queue. */
uint32_t xrecomp_host_pump_messages(void);

#ifdef __cplusplus
}
#endif

#endif /* XRECOMP_HOST_EVENTS_H */
