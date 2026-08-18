/*
 * Game-agnostic selection policy for the lifted-D3D frontend.
 *
 * This is independent of XRECOMP_GPU_BACKEND, which selects the native API
 * below Plume. The D3D frontend selects how Xbox D3D commands reach Plume:
 * reviewed native HLE hooks or the compatibility NV2A pushbuffer path.
 */
#ifndef XRECOMP_D3D_FRONTEND_H
#define XRECOMP_D3D_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XrecompD3dFrontendRequest {
    XRECOMP_D3D_FRONTEND_REQUEST_AUTO = 0,
    XRECOMP_D3D_FRONTEND_REQUEST_HLE,
    XRECOMP_D3D_FRONTEND_REQUEST_NV2A,
} XrecompD3dFrontendRequest;

typedef enum XrecompD3dFrontend {
    XRECOMP_D3D_FRONTEND_HLE = 0,
    XRECOMP_D3D_FRONTEND_NV2A,
} XrecompD3dFrontend;

typedef struct XrecompD3dFrontendReadiness {
    int config_available;
    int plan_ready;
    int runtime_ready;
    uint32_t required_hooks;
    uint32_t implemented_hooks;
    uint32_t blocker_count;
} XrecompD3dFrontendReadiness;

int xrecomp_d3d_frontend_parse(const char *text,
                               XrecompD3dFrontendRequest *request);
int xrecomp_d3d_frontend_resolve(
    XrecompD3dFrontendRequest request,
    const XrecompD3dFrontendReadiness *readiness,
    XrecompD3dFrontend *active,
    char *error,
    size_t error_capacity);

/* Applies build readiness to an explicit auto|hle|nv2a selection. */
int xrecomp_d3d_frontend_initialize_value(const char *value,
                                          char *error,
                                          size_t error_capacity);
/* Reads XEMU_D3D_FRONTEND=auto|hle|nv2a and applies build readiness. */
int xrecomp_d3d_frontend_initialize(char *error, size_t error_capacity);
XrecompD3dFrontend xrecomp_d3d_frontend_active(void);
XrecompD3dFrontendReadiness xrecomp_d3d_frontend_build_readiness(void);
const char *xrecomp_d3d_frontend_name(XrecompD3dFrontend frontend);

#ifdef __cplusplus
}
#endif

#endif /* XRECOMP_D3D_FRONTEND_H */
