#include "plume_output_quad.h"

namespace xgpu {
namespace plume {

std::array<PlumeOutputQuadVertex, 6> plumeOutputQuadVertices(
    uint32_t outputWidth, uint32_t outputHeight)
{
    const float width = static_cast<float>(outputWidth);
    const float height = static_cast<float>(outputHeight);
    return {{
        { 0.0f,  0.0f,   0.0f, 1.0f, 0.0f, 0.0f },
        { width, 0.0f,   0.0f, 1.0f, 1.0f, 0.0f },
        { width, height, 0.0f, 1.0f, 1.0f, 1.0f },
        { 0.0f,  0.0f,   0.0f, 1.0f, 0.0f, 0.0f },
        { width, height, 0.0f, 1.0f, 1.0f, 1.0f },
        { 0.0f,  height, 0.0f, 1.0f, 0.0f, 1.0f },
    }};
}

std::array<PlumeOutputQuadVertex, 6> plumeCenteredOutputQuadVertices(
    uint32_t contentWidth, uint32_t contentHeight,
    uint32_t outputWidth, uint32_t outputHeight)
{
    const float left = (static_cast<float>(outputWidth)
        - static_cast<float>(contentWidth)) * 0.5f;
    const float top = (static_cast<float>(outputHeight)
        - static_cast<float>(contentHeight)) * 0.5f;
    const float right = left + static_cast<float>(contentWidth);
    const float bottom = top + static_cast<float>(contentHeight);
    return {{
        { left,  top,    0.0f, 1.0f, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, 1.0f, 0.0f },
        { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
        { left,  top,    0.0f, 1.0f, 0.0f, 0.0f },
        { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
        { left,  bottom, 0.0f, 1.0f, 0.0f, 1.0f },
    }};
}

bool plumePresentSurfaceNeedsComposite(
    uint32_t surfaceWidth, uint32_t surfaceHeight,
    uint32_t outputWidth, uint32_t outputHeight)
{
    return surfaceWidth && surfaceHeight && outputWidth && outputHeight
        && (surfaceWidth != outputWidth || surfaceHeight != outputHeight);
}

} /* namespace plume */
} /* namespace xgpu */
