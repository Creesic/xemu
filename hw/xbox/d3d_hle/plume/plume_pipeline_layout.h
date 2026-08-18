#ifndef XGPU_PLUME_PIPELINE_LAYOUT_H
#define XGPU_PLUME_PIPELINE_LAYOUT_H

#include "plume_render_interface_types.h"

#include <array>
#include <cstdint>

namespace xgpu {
namespace plume {

struct ProgramBindingLayout {
    std::array<::plume::RenderDescriptorRange, 4> ranges;
    uint32_t rangeCount = 3;
    uint32_t rootDescriptorCount = 0;
};

struct ProgramVertexPushConstants {
    float halfWidth;
    float halfHeight;
    float homogeneous;
    float viewportZOffset;
    float viewportZScale;
    uint32_t vsConstantBase;
    float uiScaleX;
    float uiOffsetX;
    float screenSpaceOffset;
};

static_assert(sizeof(ProgramVertexPushConstants) == 9 * sizeof(float),
              "Plume vertex push constants must remain nine 32-bit scalars");

ProgramVertexPushConstants makeProgramVertexPushConstants(
    uint32_t targetWidth,
    uint32_t targetHeight,
    float homogeneous,
    float viewportZOffset,
    float viewportZScale,
    uint32_t vsConstantBase = 0,
    float uiScaleX = 1.0f,
    float uiOffsetX = 0.0f,
    float screenSpaceOffset = 0.0f);

/* D3D8 rasterizes pretransformed vertices around integer pixel centers.
 * Modern host APIs use half-integer centers, so direct XYZRHW input needs a
 * half-pixel translation.  Position mode 2 already contains the NV2A
 * viewport bias, and guest vertex programs own their viewport transform. */
float plumeScreenSpacePixelCenterOffset(uint32_t positionMode,
                                        bool hasGuestVertexShader);

ProgramBindingLayout makeProgramBindingLayout();
ProgramBindingLayout makeProgramIndexedBindingLayout();

/* Shared declaration for the screen-space transform used by Plume pipelines.
 * SPIR-V needs the explicit push-constant attribute; D3D12 uses b0. Plume's
 * Metal backend maps push-constant binding 0 to Metal buffer index 8. */
const char *ndcScaleHlslDeclaration();

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_PIPELINE_LAYOUT_H */
