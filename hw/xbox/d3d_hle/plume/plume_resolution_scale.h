/*
 * plume_resolution_scale.h — Logical/physical extent and CPU transfer helpers.
 *
 * Xbox-visible dimensions, pitches, and addresses stay logical. Plume render
 * targets may be integer-scaled physical allocations; these helpers keep that
 * conversion checked and consistent at the guest upload/readback boundary.
 */
#ifndef XGPU_PLUME_RESOLUTION_SCALE_H
#define XGPU_PLUME_RESOLUTION_SCALE_H

#include <cstdint>

namespace xgpu {
namespace plume {

static constexpr uint32_t kMinInternalResolutionScale = 1;
static constexpr uint32_t kMaxInternalResolutionScale = 10;

bool plumeValidInternalResolutionScale(uint32_t scale);
bool plumeScaledExtent(uint32_t logicalWidth, uint32_t logicalHeight,
                       uint32_t scale, uint32_t *physicalWidth,
                       uint32_t *physicalHeight);

/* Color follows Xemu's Vulkan surface-transfer policy: linear filtering in
 * both directions. Source and destination pixels are BGRA8. */
bool plumeScaleColorBgra8Linear(const uint8_t *source,
                                uint32_t sourceWidth,
                                uint32_t sourceHeight,
                                uint32_t sourcePitch,
                                uint8_t *destination,
                                uint32_t destinationWidth,
                                uint32_t destinationHeight,
                                uint32_t destinationPitch);

/* Depth follows Xemu's nearest-filter surface policy. Source values are
 * physical R32_FLOAT samples; destination receives one logical float each. */
bool plumeDownscaleDepthR32Nearest(const uint8_t *source,
                                   uint32_t physicalWidth,
                                   uint32_t physicalHeight,
                                   uint32_t sourcePitch,
                                   float *destination,
                                   uint32_t logicalWidth,
                                   uint32_t logicalHeight,
                                   uint32_t destinationPitch);

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_RESOLUTION_SCALE_H */
