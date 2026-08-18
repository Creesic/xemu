#ifndef XGPU_PLUME_SURFACE_BINDING_H
#define XGPU_PLUME_SURFACE_BINDING_H

#include "../xgpu_renderer.h"

#include <cstdint>
#include <memory>
#include <unordered_set>

namespace xgpu::plume {

struct PlumeSurfaceBindingIds {
    uint64_t color_generation = 0;
    uint64_t zeta_generation = 0;
    uint64_t framebuffer_generation = 0;
};

/* Xbox render targets and textures name guest memory, not host texture
 * generations. If a sampled generation aliases the draw target's nonzero
 * guest address, sample the current target generation so Plume's existing
 * self-sampling snapshot path observes the current image. */
uint64_t plumeNormalizeSampledSurfaceGeneration(
    uint64_t sampledGeneration, uint32_t sampledGuest,
    uint64_t targetGeneration, uint32_t targetGuest);

class PlumeSurfaceSyncTracker {
public:
    void recordWaitSubmit(bool submittedWork);
    void recordSurfaceUpload(bool uploaded, uint32_t resourceId);
    void recordHostFrameRetired(bool retired);
    bool needsPresent(bool hasQueuedWork, bool needsStickyFrame) const;
    bool hasUploadedSurface(uint32_t resourceId) const;
    void recordPresent();

private:
    bool m_submittedSincePresent = false;
    std::unordered_set<uint32_t> m_uploadedSurfaces;
};

class PlumeSurfaceBindingTracker {
public:
    PlumeSurfaceBindingTracker();
    ~PlumeSurfaceBindingTracker();
    PlumeSurfaceBindingTracker(PlumeSurfaceBindingTracker &&) noexcept;
    PlumeSurfaceBindingTracker &operator=(PlumeSurfaceBindingTracker &&) noexcept;

    PlumeSurfaceBindingTracker(const PlumeSurfaceBindingTracker &) = delete;
    PlumeSurfaceBindingTracker &operator=(const PlumeSurfaceBindingTracker &) = delete;

    PlumeSurfaceBindingIds bind(const XgpuSurfaceBinding &binding);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace xgpu::plume

#endif
