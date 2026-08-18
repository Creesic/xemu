#include "plume_resolution_scale.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace xgpu {
namespace plume {

bool plumeValidInternalResolutionScale(uint32_t scale)
{
    return scale >= kMinInternalResolutionScale &&
           scale <= kMaxInternalResolutionScale;
}

bool plumeScaledExtent(uint32_t logicalWidth, uint32_t logicalHeight,
                       uint32_t scale, uint32_t *physicalWidth,
                       uint32_t *physicalHeight)
{
    if (!physicalWidth || !physicalHeight || !logicalWidth ||
        !logicalHeight || !plumeValidInternalResolutionScale(scale) ||
        logicalWidth > std::numeric_limits<uint32_t>::max() / scale ||
        logicalHeight > std::numeric_limits<uint32_t>::max() / scale)
        return false;
    *physicalWidth = logicalWidth * scale;
    *physicalHeight = logicalHeight * scale;
    return true;
}

bool plumeScaleColorBgra8Linear(const uint8_t *source,
                                uint32_t sourceWidth,
                                uint32_t sourceHeight,
                                uint32_t sourcePitch,
                                uint8_t *destination,
                                uint32_t destinationWidth,
                                uint32_t destinationHeight,
                                uint32_t destinationPitch)
{
    if (!source || !destination || !sourceWidth || !sourceHeight ||
        !destinationWidth || !destinationHeight ||
        sourceWidth > UINT32_MAX / 4u ||
        destinationWidth > UINT32_MAX / 4u ||
        sourcePitch < sourceWidth * 4u ||
        destinationPitch < destinationWidth * 4u)
        return false;

    if (sourceWidth == destinationWidth && sourceHeight == destinationHeight) {
        for (uint32_t y = 0; y < sourceHeight; ++y)
            std::memcpy(destination + static_cast<size_t>(y) * destinationPitch,
                        source + static_cast<size_t>(y) * sourcePitch,
                        static_cast<size_t>(sourceWidth) * 4u);
        return true;
    }

    const double scaleX = static_cast<double>(sourceWidth) / destinationWidth;
    const double scaleY = static_cast<double>(sourceHeight) / destinationHeight;
    for (uint32_t y = 0; y < destinationHeight; ++y) {
        const double sourceY = (static_cast<double>(y) + 0.5) * scaleY - 0.5;
        const int64_t rawY0 = static_cast<int64_t>(std::floor(sourceY));
        const int64_t rawY1 = rawY0 + 1;
        const uint32_t y0 = static_cast<uint32_t>(std::clamp<int64_t>(
            rawY0, 0, static_cast<int64_t>(sourceHeight) - 1));
        const uint32_t y1 = static_cast<uint32_t>(std::clamp<int64_t>(
            rawY1, 0, static_cast<int64_t>(sourceHeight) - 1));
        const double fy = sourceY - std::floor(sourceY);
        const uint8_t *row0 = source + static_cast<size_t>(y0) * sourcePitch;
        const uint8_t *row1 = source + static_cast<size_t>(y1) * sourcePitch;
        uint8_t *out = destination + static_cast<size_t>(y) * destinationPitch;

        for (uint32_t x = 0; x < destinationWidth; ++x) {
            const double sourceX =
                (static_cast<double>(x) + 0.5) * scaleX - 0.5;
            const int64_t rawX0 = static_cast<int64_t>(std::floor(sourceX));
            const int64_t rawX1 = rawX0 + 1;
            const uint32_t x0 = static_cast<uint32_t>(std::clamp<int64_t>(
                rawX0, 0, static_cast<int64_t>(sourceWidth) - 1));
            const uint32_t x1 = static_cast<uint32_t>(std::clamp<int64_t>(
                rawX1, 0, static_cast<int64_t>(sourceWidth) - 1));
            const double fx = sourceX - std::floor(sourceX);
            for (uint32_t channel = 0; channel < 4; ++channel) {
                const double top = row0[x0 * 4u + channel] * (1.0 - fx) +
                                   row0[x1 * 4u + channel] * fx;
                const double bottom = row1[x0 * 4u + channel] * (1.0 - fx) +
                                      row1[x1 * 4u + channel] * fx;
                const double value = top * (1.0 - fy) + bottom * fy;
                out[x * 4u + channel] = static_cast<uint8_t>(
                    std::clamp(value + 0.5, 0.0, 255.0));
            }
        }
    }
    return true;
}

bool plumeDownscaleDepthR32Nearest(const uint8_t *source,
                                   uint32_t physicalWidth,
                                   uint32_t physicalHeight,
                                   uint32_t sourcePitch,
                                   float *destination,
                                   uint32_t logicalWidth,
                                   uint32_t logicalHeight,
                                   uint32_t destinationPitch)
{
    if (!source || !destination || !physicalWidth || !physicalHeight ||
        !logicalWidth || !logicalHeight ||
        physicalWidth < logicalWidth || physicalHeight < logicalHeight ||
        physicalWidth % logicalWidth || physicalHeight % logicalHeight ||
        physicalWidth > UINT32_MAX / 4u || logicalWidth > UINT32_MAX / 4u ||
        sourcePitch < physicalWidth * 4u ||
        destinationPitch < logicalWidth * sizeof(float))
        return false;

    const uint32_t scaleX = physicalWidth / logicalWidth;
    const uint32_t scaleY = physicalHeight / logicalHeight;
    for (uint32_t y = 0; y < logicalHeight; ++y) {
        const uint32_t sourceY = std::min(
            y * scaleY + scaleY / 2u, physicalHeight - 1u);
        const float *sourceRow = reinterpret_cast<const float *>(
            source + static_cast<size_t>(sourceY) * sourcePitch);
        float *destinationRow = reinterpret_cast<float *>(
            reinterpret_cast<uint8_t *>(destination) +
            static_cast<size_t>(y) * destinationPitch);
        for (uint32_t x = 0; x < logicalWidth; ++x) {
            const uint32_t sourceX = std::min(
                x * scaleX + scaleX / 2u, physicalWidth - 1u);
            destinationRow[x] = sourceRow[sourceX];
        }
    }
    return true;
}

} /* namespace plume */
} /* namespace xgpu */
