#ifndef XGPU_PLUME_UI_CANVAS_H
#define XGPU_PLUME_UI_CANVAS_H

namespace xgpu {
namespace plume {

struct PlumeUiCanvasHorizontalTransform {
    float scale = 1.0f;
    float offset = 0.0f;
    bool stretches = false;
};

/*
 * Build one horizontal transform for a UI draw. Small draws retain their
 * pixel size and anchor left, center, or right according to their logical
 * canvas region. Draws spanning most of the canvas stretch to cover the new
 * output, which keeps full-screen panels and fades edge-to-edge. Centered mode
 * instead preserves the complete logical canvas at native size.
 */
bool plumeComputeUiCanvasHorizontalTransform(
    float minimumX,
    float maximumX,
    float logicalWidth,
    float targetWidth,
    float viewportX,
    float viewportWidth,
    bool sourceWasViewportTransformed,
    bool centerCanvas,
    PlumeUiCanvasHorizontalTransform &transform);

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_UI_CANVAS_H */
