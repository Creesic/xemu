#ifndef D3D8_CPU_SURFACE_SYNC_H
#define D3D8_CPU_SURFACE_SYNC_H

#include <stddef.h>
#include <stdint.h>

typedef struct D3D8CpuSurfaceFingerprint {
    uint64_t hash;
    size_t nonzero_bytes;
} D3D8CpuSurfaceFingerprint;

static inline D3D8CpuSurfaceFingerprint d3d8_cpu_surface_fingerprint(
    const void *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t pitch)
{
    const uint8_t *base = (const uint8_t *)pixels;
    const size_t active_row_bytes = (size_t)width * 4u;
    D3D8CpuSurfaceFingerprint result = {
        UINT64_C(14695981039346656037),
        0,
    };

    if (!base || active_row_bytes > pitch) {
        return result;
    }

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = base + ((size_t)y * pitch);

        for (size_t x = 0; x < active_row_bytes; ++x) {
            const uint8_t value = row[x];

            result.hash ^= value;
            result.hash *= UINT64_C(1099511628211);
            result.nonzero_bytes += value != 0;
        }
    }

    return result;
}

static inline int d3d8_cpu_surface_needs_upload(
    int previous_hash_valid,
    uint64_t previous_hash,
    D3D8CpuSurfaceFingerprint current,
    int writable_cpu_lock)
{
    if (writable_cpu_lock)
        return 1;
    if (!previous_hash_valid) {
        return current.nonzero_bytes != 0;
    }

    return previous_hash != current.hash;
}

static inline int d3d8_cpu_surface_format_is_32bpp(uint32_t format)
{
    return (format >= 4 && format <= 8) ||
           format == UINT32_C(0x12) || /* D3DFMT_LIN_A8R8G8B8 */
           format == UINT32_C(0x1E);   /* D3DFMT_LIN_X8R8G8B8 */
}

static inline uint32_t d3d8_cpu_surface_physical_offset(
    uint32_t guest_address)
{
    if (guest_address >= UINT32_C(0xF0000000) &&
        guest_address < UINT32_C(0xFD000000))
        return guest_address % UINT32_C(0x04000000);
    return guest_address;
}

static inline int d3d8_cpu_surface_lock_is_writable(uint32_t lock_flags)
{
    return (lock_flags & UINT32_C(0x00000010)) == 0;
}

static inline int d3d8_cpu_surface_lock_needs_readback(
    uint32_t lock_flags,
    int hosted_surface_known)
{
    return hosted_surface_known &&
           d3d8_cpu_surface_lock_is_writable(lock_flags);
}

#ifdef __cplusplus
extern "C" {
#endif

void d3d8_PgraphMarkCpuSurfaceLock(uint32_t guest_address,
                                   uint32_t lock_flags,
                                   int preserve_scanout);

#ifdef __cplusplus
}
#endif

#endif
