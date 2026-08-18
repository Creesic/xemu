#include "plume_ui_canvas.h"

#include <algorithm>
#include <cmath>

namespace xgpu {
namespace plume {

bool plumeComputeUiCanvasHorizontalTransform(
    float minimumX,
    float maximumX,
    float logicalWidth,
    float targetWidth,
    float viewportX,
    float viewportWidth,
    bool sourceWasViewportTransformed,
    bool centerCanvas,
    PlumeUiCanvasHorizontalTransform &transform)
{
    float sourceScale = 1.0f;
    float sourceOffset = 0.0f;
    float logicalMinimum;
    float logicalMaximum;
    float span;
    float center;
    float anchor;

    transform = {};
    if (!std::isfinite(minimumX) || !std::isfinite(maximumX)
        || !std::isfinite(logicalWidth) || !std::isfinite(targetWidth)
        || !std::isfinite(viewportX) || !std::isfinite(viewportWidth)
        || logicalWidth <= 0.0f || targetWidth <= 0.0f
        || (sourceWasViewportTransformed && viewportWidth <= 0.0f)) {
        return false;
    }
    if (minimumX > maximumX)
        std::swap(minimumX, maximumX);
    if (sourceWasViewportTransformed) {
        sourceScale = logicalWidth / viewportWidth;
        sourceOffset = -viewportX * sourceScale;
    }

    logicalMinimum = minimumX * sourceScale + sourceOffset;
    logicalMaximum = maximumX * sourceScale + sourceOffset;
    span = logicalMaximum - logicalMinimum;
    center = (logicalMinimum + logicalMaximum) * 0.5f;

    if (centerCanvas) {
        transform.scale = sourceScale;
        transform.offset = sourceOffset
            + (targetWidth - logicalWidth) * 0.5f;
        return true;
    }

    /* Full-width panels, fades, and backgrounds should still cover the frame. */
    if (span >= logicalWidth * 0.75f) {
        const float outputScale = targetWidth / logicalWidth;
        transform.scale = sourceScale * outputScale;
        transform.offset = sourceOffset * outputScale;
        transform.stretches = true;
        return true;
    }

    /* UI widgets are laid out in three conventional anchoring regions. */
    if (center <= logicalWidth / 3.0f)
        anchor = 0.0f;
    else if (center >= logicalWidth * (2.0f / 3.0f))
        anchor = 1.0f;
    else
        anchor = 0.5f;

    transform.scale = sourceScale;
    transform.offset = sourceOffset + (targetWidth - logicalWidth) * anchor;
    return true;
}

} /* namespace plume */
} /* namespace xgpu */
