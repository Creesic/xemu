#ifndef XRECOMP_PLUME_SURFACE_DOWNLOAD_H
#define XRECOMP_PLUME_SURFACE_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Plume renders color targets as BGRA8 even when the HLE D3D surface exposed
 * to the guest is narrower. Native PGRAPH surfaces use NV097 color-format
 * values (7/8 for the supported 32-bit targets); HLE surfaces retain their
 * Xbox D3DFORMAT value (0 for L8, 0x19 for A8).
 */
static inline uint32_t xgpu_plume_guest_color_row_bytes(
    uint32_t guest_format, uint32_t width)
{
    if (guest_format == 0u || guest_format == 0x19u)
        return width; /* Xbox D3DFMT_L8 / D3DFMT_A8 */
    if (guest_format == 7u || guest_format == 8u) {
        if (width > UINT32_MAX / 4u)
            return 0;
        return width * 4u;
    }
    return 0;
}

static inline int xgpu_plume_pack_guest_color_surface(
    uint32_t guest_format, const uint8_t *bgra, uint32_t bgra_pitch,
    uint8_t *guest, uint32_t guest_pitch, uint32_t width, uint32_t height)
{
    uint32_t row;
    uint32_t guest_row_bytes =
        xgpu_plume_guest_color_row_bytes(guest_format, width);

    if (!bgra || !guest || !width || !height || !guest_row_bytes ||
        width > UINT32_MAX / 4u || bgra_pitch < width * 4u ||
        guest_pitch < guest_row_bytes)
        return 0;

    for (row = 0; row < height; ++row) {
        const uint8_t *src = bgra + (size_t)row * bgra_pitch;
        uint8_t *dst = guest + (size_t)row * guest_pitch;
        uint32_t x;

        if (guest_format != 0u && guest_format != 0x19u) {
            for (x = 0; x < guest_row_bytes; ++x)
                dst[x] = src[x];
            continue;
        }

        /*
         * The translated Xbox combiner keeps the single-channel L8 render
         * target value in its output alpha. Packing alpha here restores the
         * byte layout that the guest observes through Surface::LockRect.
         */
        for (x = 0; x < width; ++x)
            dst[x] = src[x * 4u + 3u];
    }
    return 1;
}

#endif /* XRECOMP_PLUME_SURFACE_DOWNLOAD_H */
