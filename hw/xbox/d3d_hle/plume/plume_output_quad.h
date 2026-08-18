#ifndef XGPU_PLUME_OUTPUT_QUAD_H
#define XGPU_PLUME_OUTPUT_QUAD_H

#include <array>
#include <cstdint>

namespace xgpu {
namespace plume {

struct PlumeOutputQuadVertex {
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
};

std::array<PlumeOutputQuadVertex, 6> plumeOutputQuadVertices(
    uint32_t outputWidth, uint32_t outputHeight);

std::array<PlumeOutputQuadVertex, 6> plumeCenteredOutputQuadVertices(
    uint32_t contentWidth, uint32_t contentHeight,
    uint32_t outputWidth, uint32_t outputHeight);

bool plumePresentSurfaceNeedsComposite(
    uint32_t surfaceWidth, uint32_t surfaceHeight,
    uint32_t outputWidth, uint32_t outputHeight);

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_OUTPUT_QUAD_H */
