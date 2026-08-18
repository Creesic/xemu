#include "d3d_frontend.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XRECOMP_D3D_HLE_CONFIG_AVAILABLE
#define XRECOMP_D3D_HLE_CONFIG_AVAILABLE 0
#endif
#ifndef XRECOMP_D3D_HLE_PLAN_READY
#define XRECOMP_D3D_HLE_PLAN_READY 0
#endif
#ifndef XRECOMP_D3D_HLE_REQUIRED_HOOKS
#define XRECOMP_D3D_HLE_REQUIRED_HOOKS 0
#endif
#ifndef XRECOMP_D3D_HLE_IMPLEMENTED_HOOKS
#define XRECOMP_D3D_HLE_IMPLEMENTED_HOOKS 0
#endif
#ifndef XRECOMP_D3D_HLE_BLOCKER_COUNT
#define XRECOMP_D3D_HLE_BLOCKER_COUNT 1
#endif

/*
 * The planner proves per-XBE identity and implementation coverage. This
 * independent runtime gate remains false until native guest-ABI entry shims
 * and their lifetime/order contracts are actually connected.
 */
#ifndef XRECOMP_D3D_HLE_RUNTIME_READY
#define XRECOMP_D3D_HLE_RUNTIME_READY 0
#endif

static XrecompD3dFrontend g_active = XRECOMP_D3D_FRONTEND_NV2A;

static int equal_ignore_case(const char *left, const char *right)
{
    unsigned char a;
    unsigned char b;
    if (!left || !right)
        return 0;
    do {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (tolower(a) != tolower(b))
            return 0;
    } while (a && b);
    return a == b;
}

static void set_error(char *error, size_t capacity, const char *message)
{
    if (!error || !capacity)
        return;
    snprintf(error, capacity, "%s", message ? message : "");
}

int xrecomp_d3d_frontend_parse(const char *text,
                               XrecompD3dFrontendRequest *request)
{
    if (!request)
        return 0;
    if (!text || !text[0] || equal_ignore_case(text, "nv2a"))
        *request = XRECOMP_D3D_FRONTEND_REQUEST_NV2A;
    else if (equal_ignore_case(text, "auto"))
        *request = XRECOMP_D3D_FRONTEND_REQUEST_AUTO;
    else if (equal_ignore_case(text, "hle"))
        *request = XRECOMP_D3D_FRONTEND_REQUEST_HLE;
    else if (equal_ignore_case(text, "nv2a"))
        *request = XRECOMP_D3D_FRONTEND_REQUEST_NV2A;
    else
        return 0;
    return 1;
}

int xrecomp_d3d_frontend_resolve(
    XrecompD3dFrontendRequest request,
    const XrecompD3dFrontendReadiness *readiness,
    XrecompD3dFrontend *active,
    char *error,
    size_t error_capacity)
{
    int hle_ready;
    if (!readiness || !active) {
        set_error(error, error_capacity,
                  "D3D frontend resolver received no readiness state");
        return 0;
    }
    hle_ready = readiness->config_available
             && readiness->plan_ready
             && readiness->runtime_ready
             && readiness->implemented_hooks == readiness->required_hooks
             && readiness->blocker_count == 0;

    switch (request) {
    case XRECOMP_D3D_FRONTEND_REQUEST_AUTO:
        *active = hle_ready
            ? XRECOMP_D3D_FRONTEND_HLE
            : XRECOMP_D3D_FRONTEND_NV2A;
        set_error(error, error_capacity, "");
        return 1;
    case XRECOMP_D3D_FRONTEND_REQUEST_NV2A:
        *active = XRECOMP_D3D_FRONTEND_NV2A;
        set_error(error, error_capacity, "");
        return 1;
    case XRECOMP_D3D_FRONTEND_REQUEST_HLE:
        if (hle_ready) {
            *active = XRECOMP_D3D_FRONTEND_HLE;
            set_error(error, error_capacity, "");
            return 1;
        }
        if (error && error_capacity) {
            snprintf(
                error, error_capacity,
                "HLE was forced but is not ready "
                "(config=%d plan=%d runtime=%d hooks=%u/%u blockers=%u); "
                "use XRECOMP_D3D_FRONTEND=nv2a or auto",
                readiness->config_available,
                readiness->plan_ready,
                readiness->runtime_ready,
                readiness->implemented_hooks,
                readiness->required_hooks,
                readiness->blocker_count);
        }
        return 0;
    }

    set_error(error, error_capacity, "invalid D3D frontend request");
    return 0;
}

XrecompD3dFrontendReadiness xrecomp_d3d_frontend_build_readiness(void)
{
    XrecompD3dFrontendReadiness readiness;
    readiness.config_available = XRECOMP_D3D_HLE_CONFIG_AVAILABLE;
    readiness.plan_ready = XRECOMP_D3D_HLE_PLAN_READY;
    readiness.runtime_ready = XRECOMP_D3D_HLE_RUNTIME_READY;
    readiness.required_hooks = XRECOMP_D3D_HLE_REQUIRED_HOOKS;
    readiness.implemented_hooks = XRECOMP_D3D_HLE_IMPLEMENTED_HOOKS;
    readiness.blocker_count = XRECOMP_D3D_HLE_BLOCKER_COUNT;
    return readiness;
}

int xrecomp_d3d_frontend_initialize_value(const char *value,
                                          char *error,
                                          size_t error_capacity)
{
    XrecompD3dFrontendRequest request;
    XrecompD3dFrontendReadiness readiness;

    if (!xrecomp_d3d_frontend_parse(value, &request)) {
        if (error && error_capacity) {
            snprintf(error, error_capacity,
                     "invalid XEMU_D3D_FRONTEND=%s; "
                     "expected auto, hle, or nv2a",
                     value ? value : "");
        }
        return 0;
    }

    readiness = xrecomp_d3d_frontend_build_readiness();
    return xrecomp_d3d_frontend_resolve(
        request, &readiness, &g_active, error, error_capacity);
}

int xrecomp_d3d_frontend_initialize(char *error, size_t error_capacity)
{
    return xrecomp_d3d_frontend_initialize_value(
        getenv("XEMU_D3D_FRONTEND"), error, error_capacity);
}

XrecompD3dFrontend xrecomp_d3d_frontend_active(void)
{
    return g_active;
}

const char *xrecomp_d3d_frontend_name(XrecompD3dFrontend frontend)
{
    switch (frontend) {
    case XRECOMP_D3D_FRONTEND_HLE:
        return "hle";
    case XRECOMP_D3D_FRONTEND_NV2A:
        return "nv2a";
    }
    return "unknown";
}
