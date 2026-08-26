#include "plume_render_state.h"

namespace xgpu::plume {

using namespace ::plume;

static RenderBlend plume_blend_from_d3d(uint32_t blend)
{
    switch (blend) {
    case 1: return RenderBlend::ZERO;
    case 2: return RenderBlend::ONE;
    case 3: return RenderBlend::SRC_COLOR;
    case 4: return RenderBlend::INV_SRC_COLOR;
    case 5: return RenderBlend::SRC_ALPHA;
    case 6: return RenderBlend::INV_SRC_ALPHA;
    case 7: return RenderBlend::DEST_ALPHA;
    case 8: return RenderBlend::INV_DEST_ALPHA;
    case 9: return RenderBlend::DEST_COLOR;
    case 10: return RenderBlend::INV_DEST_COLOR;
    case 11: return RenderBlend::SRC_ALPHA_SAT;
    case 12: return RenderBlend::BLEND_FACTOR;
    case 13: return RenderBlend::INV_BLEND_FACTOR;
    case 14: return RenderBlend::BLEND_FACTOR_ALPHA;
    case 15: return RenderBlend::INV_BLEND_FACTOR_ALPHA;
    default: return RenderBlend::ONE;
    }
}

static RenderBlend plume_alpha_blend_from_d3d(uint32_t blend)
{
    switch (blend) {
    case 3: return RenderBlend::SRC_ALPHA;
    case 4: return RenderBlend::INV_SRC_ALPHA;
    case 9: return RenderBlend::DEST_ALPHA;
    case 10: return RenderBlend::INV_DEST_ALPHA;
    case 11: return RenderBlend::ONE;
    default: return plume_blend_from_d3d(blend);
    }
}

static RenderBlendOperation plume_blend_op_from_d3d(uint32_t operation)
{
    switch (operation) {
    case 2: return RenderBlendOperation::SUBTRACT;
    case 3: return RenderBlendOperation::REV_SUBTRACT;
    case 4: return RenderBlendOperation::MIN;
    case 5: return RenderBlendOperation::MAX;
    default: return RenderBlendOperation::ADD;
    }
}

RenderBlendDesc plume_blend_desc_from_d3d(const XgpuPlumeRenderState &state)
{
    RenderBlendDesc blend = RenderBlendDesc::Copy();
    blend.blendEnabled = state.blend_enable != 0;
    blend.srcBlend = plume_blend_from_d3d(state.src_blend);
    blend.dstBlend = plume_blend_from_d3d(state.dst_blend);
    blend.blendOp = plume_blend_op_from_d3d(state.blend_op);
    blend.srcBlendAlpha = plume_alpha_blend_from_d3d(state.src_blend);
    blend.dstBlendAlpha = plume_alpha_blend_from_d3d(state.dst_blend);
    blend.blendOpAlpha = plume_blend_op_from_d3d(state.blend_op);
    blend.renderTargetWriteMask =
        static_cast<uint8_t>(state.color_write_mask & 0xFu);
    return blend;
}

RenderColor plume_blend_factor_from_xgpu(uint32_t color)
{
    constexpr float scale = 1.0f / 255.0f;
    return RenderColor(float((color >> 16) & 0xFFu) * scale,
                       float((color >> 8) & 0xFFu) * scale,
                       float(color & 0xFFu) * scale,
                       float((color >> 24) & 0xFFu) * scale);
}

RenderFormat plume_depth_format_from_xgpu(uint32_t zetaFormat,
                                          uint32_t zetaFloat)
{
    switch (zetaFormat) {
    case XGPU_ZETA_Z16:
        return zetaFloat ? RenderFormat::D32_FLOAT
                         : RenderFormat::D16_UNORM;
    case XGPU_ZETA_Z24S8:
        return RenderFormat::D32_FLOAT_S8_UINT;
    default:
        return RenderFormat::UNKNOWN;
    }
}

PlumeViewportTransform plume_viewport_transform(
    const XgpuPlumeRenderState &state)
{
    /*
     * Xbox D3D reserves c[-38] and c[-37] (physical constant slots 58 and
     * 59) for the viewport scale and offset.  The values below match the XDK
     * CommonSetViewport path for the currently supported non-multisampled
     * surface contract.  Floating zeta formats use the NV2A representable
     * maxima documented by the hardware conversion path.
     */
    float zMax;
    switch (state.zeta_format) {
    case XGPU_ZETA_Z16:
        zMax = state.zeta_float ? 511.9375f : 65535.0f;
        break;
    case XGPU_ZETA_Z24S8:
        zMax = state.zeta_float ? 1.0e30f : 16777215.0f;
        break;
    default:
        zMax = 16777215.0f;
        break;
    }

    const float halfWidth = static_cast<float>(state.viewport_width) * 0.5f;
    const float halfHeight = static_cast<float>(state.viewport_height) * 0.5f;
    PlumeViewportTransform transform = {};
    transform.scale[0] = halfWidth;
    transform.scale[1] = -halfHeight;
    transform.scale[2] =
        (state.viewport_max_z - state.viewport_min_z) * zMax;
    transform.offset[0] =
        static_cast<float>(state.viewport_x) + halfWidth + 0.53125f;
    transform.offset[1] =
        static_cast<float>(state.viewport_y) + halfHeight + 0.53125f;
    transform.offset[2] = state.viewport_min_z * zMax;
    return transform;
}

static RenderComparisonFunction plume_compare_from_d3d(uint32_t func)
{
    if (func >= 1 && func <= 8)
        return static_cast<RenderComparisonFunction>(func);
    return RenderComparisonFunction::ALWAYS;
}

static RenderStencilOp plume_stencil_op_from_d3d(uint32_t op)
{
    if (op >= 1 && op <= 8)
        return static_cast<RenderStencilOp>(op);
    return RenderStencilOp::KEEP;
}

void plume_apply_stencil_state(RenderGraphicsPipelineDesc &desc,
                               const XgpuPlumeRenderState &state)
{
    RenderStencilFaceDesc face;
    face.compareFunction = plume_compare_from_d3d(state.stencil_func);
    face.failOp = plume_stencil_op_from_d3d(state.stencil_fail);
    face.depthFailOp = plume_stencil_op_from_d3d(state.stencil_zfail);
    face.passOp = plume_stencil_op_from_d3d(state.stencil_pass);

    desc.stencilEnabled = state.stencil_enable != 0;
    desc.stencilReadMask = state.stencil_read_mask & 0xFFu;
    desc.stencilWriteMask = state.stencil_write_mask & 0xFFu;
    desc.stencilReference = state.stencil_ref & 0xFFu;
    desc.stencilFrontFace = face;
    desc.stencilBackFace = face;
}

uint64_t plume_render_state_key(const XgpuPlumeRenderState &state)
{
    uint64_t key = 1469598103934665603ull;
    const uint32_t fields[] = {
        state.depth_enable, state.z_perspective,
        state.depth_write, state.depth_func,
        state.blend_enable, state.src_blend, state.dst_blend, state.blend_op,
        state.blend_color,
        state.cull_mode, state.color_write_mask,
        state.zeta_format, state.zeta_float,
        state.stencil_enable, state.stencil_func, state.stencil_ref,
        state.stencil_read_mask, state.stencil_write_mask,
        state.stencil_fail, state.stencil_zfail, state.stencil_pass,
        state.depth_bias_bits, state.slope_scaled_depth_bias_bits,
    };
    for (uint32_t field : fields) {
        key ^= field;
        key *= 1099511628211ull;
    }
    return key;
}

void plume_make_clear_quad(const XgpuRect *rect, uint32_t target_width,
                           uint32_t target_height, uint32_t diffuse,
                           PlumeClearVertex vertices[6])
{
    const float left = rect ? float(rect->x) : 0.0f;
    const float top = rect ? float(rect->y) : 0.0f;
    const float right = rect ? float(rect->x + rect->width)
                             : float(target_width);
    const float bottom = rect ? float(rect->y + rect->height)
                              : float(target_height);
    const PlumeClearVertex quad[6] = {
        { left,  top,    0.0f, 1.0f, diffuse },
        { right, top,    0.0f, 1.0f, diffuse },
        { left,  bottom, 0.0f, 1.0f, diffuse },
        { left,  bottom, 0.0f, 1.0f, diffuse },
        { right, top,    0.0f, 1.0f, diffuse },
        { right, bottom, 0.0f, 1.0f, diffuse },
    };
    if (vertices) {
        for (uint32_t i = 0; i < 6; i++)
            vertices[i] = quad[i];
    }
}

} // namespace xgpu::plume
