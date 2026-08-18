#ifndef XGPU_PLUME_RENDER_STATE_H
#define XGPU_PLUME_RENDER_STATE_H

#include "plume_render_interface.h"
#include "plume_host.h"
#include "../xgpu_renderer.h"

namespace xgpu::plume {

struct PlumeClearVertex {
    float x;
    float y;
    float z;
    float rhw;
    uint32_t diffuse;
};

struct PlumeViewportTransform {
    float scale[4];
    float offset[4];
};

::plume::RenderBlendDesc plume_blend_desc_from_d3d(
    const XgpuPlumeRenderState &state);

::plume::RenderFormat plume_depth_format_from_xgpu(uint32_t zetaFormat,
                                                    uint32_t zetaFloat);

PlumeViewportTransform plume_viewport_transform(
    const XgpuPlumeRenderState &state);

void plume_apply_stencil_state(::plume::RenderGraphicsPipelineDesc &desc,
                               const XgpuPlumeRenderState &state);

uint64_t plume_render_state_key(const XgpuPlumeRenderState &state);

void plume_make_clear_quad(const XgpuRect *rect, uint32_t target_width,
                           uint32_t target_height, uint32_t diffuse,
                           PlumeClearVertex vertices[6]);

} // namespace xgpu::plume

#endif
