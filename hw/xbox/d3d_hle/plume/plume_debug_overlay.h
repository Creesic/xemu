/*
 * Validation and screen-space geometry for host RGBA debug overlays.
 * This stays title-neutral; game-specific diagnostics only provide pixels.
 */
#ifndef XGPU_PLUME_DEBUG_OVERLAY_H
#define XGPU_PLUME_DEBUG_OVERLAY_H

#include "plume_host.h"

#include <array>
#include <cstdint>

namespace xgpu::plume {

struct PlumeDebugOverlayVertex {
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
};

inline uint32_t plumeDebugOverlayTextureGuest(uint32_t slot)
{
    return 0xFFFFFFFDu - slot;
}

inline bool plumeDebugOverlayFrameValid(
    const XgpuPlumeDebugOverlayFrame &frame,
    uint32_t panelWidth,
    uint32_t panelHeight)
{
    if (!frame.pixels || !frame.width || !frame.height || !frame.pitch ||
        !panelWidth || !panelHeight)
        return false;
    if (frame.x >= panelWidth || frame.y >= panelHeight ||
        frame.width > panelWidth - frame.x ||
        frame.height > panelHeight - frame.y)
        return false;
    return static_cast<uint64_t>(frame.pitch) >=
           static_cast<uint64_t>(frame.width) * 4u;
}

inline std::array<PlumeDebugOverlayVertex, 6>
plumeDebugOverlayVertices(const XgpuPlumeDebugOverlayFrame &frame)
{
    const float left = static_cast<float>(frame.x);
    const float top = static_cast<float>(frame.y);
    const float right = static_cast<float>(frame.x + frame.width);
    const float bottom = static_cast<float>(frame.y + frame.height);
    return {{
        {left,  top,    0.0f, 1.0f, 0.0f, 0.0f},
        {right, top,    0.0f, 1.0f, 1.0f, 0.0f},
        {right, bottom, 0.0f, 1.0f, 1.0f, 1.0f},
        {left,  top,    0.0f, 1.0f, 0.0f, 0.0f},
        {right, bottom, 0.0f, 1.0f, 1.0f, 1.0f},
        {left,  bottom, 0.0f, 1.0f, 0.0f, 1.0f},
    }};
}

inline bool plumeDebugOverlayShouldCopyPresentBeforeDraw(
    bool drawAfterPresentCopy,
    bool copyPresentSurface,
    bool hasPresentTarget,
    bool presentAlreadyCopied)
{
    return drawAfterPresentCopy && copyPresentSurface && hasPresentTarget &&
           !presentAlreadyCopied;
}

} /* namespace xgpu::plume */

#endif /* XGPU_PLUME_DEBUG_OVERLAY_H */
