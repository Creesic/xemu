#ifndef XRECOMP_D3D_HLE_GUEST_DRIVER_H
#define XRECOMP_D3D_HLE_GUEST_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*d3d_hle_guest_driver_prepare_callback)(void);

enum d3d_hle_guest_driver_requirement {
    D3D_HLE_GUEST_DRIVER_NEEDS_NV2A_CONTROL_PLANE = 1u << 0,
};

void d3d_hle_guest_driver_set_prepare_callback(
    d3d_hle_guest_driver_prepare_callback callback,
    unsigned requirements);
unsigned d3d_hle_guest_driver_requirements(void);
int d3d_hle_guest_driver_prepare(void);

#ifdef __cplusplus
}
#endif

#endif /* XRECOMP_D3D_HLE_GUEST_DRIVER_H */
