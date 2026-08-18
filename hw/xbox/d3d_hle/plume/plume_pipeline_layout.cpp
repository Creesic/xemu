#include "plume_pipeline_layout.h"

namespace xgpu {
namespace plume {

ProgramVertexPushConstants makeProgramVertexPushConstants(
    uint32_t targetWidth,
    uint32_t targetHeight,
    float homogeneous,
    float viewportZOffset,
    float viewportZScale,
    uint32_t vsConstantBase,
    float uiScaleX,
    float uiOffsetX,
    float screenSpaceOffset)
{
    return {
        targetWidth > 0 ? float(targetWidth) * 0.5f : 320.0f,
        targetHeight > 0 ? float(targetHeight) * 0.5f : 240.0f,
        homogeneous,
        viewportZOffset,
        viewportZScale,
        vsConstantBase,
        uiScaleX,
        uiOffsetX,
        screenSpaceOffset,
    };
}

float plumeScreenSpacePixelCenterOffset(uint32_t positionMode,
                                        bool hasGuestVertexShader)
{
    return !hasGuestVertexShader && positionMode == 1u ? 0.5f : 0.0f;
}

ProgramBindingLayout makeProgramBindingLayout()
{
    ProgramBindingLayout layout = {{
        ::plume::RenderDescriptorRange(
            ::plume::RenderDescriptorRangeType::TEXTURE, 0, 4),
        ::plume::RenderDescriptorRange(
            ::plume::RenderDescriptorRangeType::SAMPLER, 4, 4),
        ::plume::RenderDescriptorRange(
            ::plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 8, 1),
    }};
    return layout;
}

ProgramBindingLayout makeProgramIndexedBindingLayout()
{
    ProgramBindingLayout layout = makeProgramBindingLayout();
    layout.ranges[3] = ::plume::RenderDescriptorRange(
        ::plume::RenderDescriptorRangeType::STRUCTURED_BUFFER, 4, 1);
    layout.rangeCount = 4;
    return layout;
}

const char *ndcScaleHlslDeclaration()
{
    return
        "struct NdcScaleData { float halfW; float halfH; float homogeneous; "
        "float viewportZOffset; float viewportZScale; "
        "uint vsConstantBase; float uiScaleX; float uiOffsetX; "
        "float screenSpaceOffset; };\n"
        "#ifdef XGPU_SPIRV\n"
        "[[vk::push_constant]] ConstantBuffer<NdcScaleData> ndcScale;\n"
        "#else\n"
        "ConstantBuffer<NdcScaleData> ndcScale : register(b0);\n"
        "#endif\n";
}

} /* namespace plume */
} /* namespace xgpu */
