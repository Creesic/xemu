#include "plume_surface_binding.h"

#include <map>
#include <tuple>
#include <utility>

namespace xgpu::plume {

uint64_t plumeNormalizeSampledSurfaceGeneration(
    uint64_t sampledGeneration, uint32_t sampledGuest,
    uint64_t targetGeneration, uint32_t targetGuest)
{
    if (sampledGeneration && targetGeneration && sampledGuest &&
        sampledGuest == targetGuest)
        return targetGeneration;
    return sampledGeneration;
}

void PlumeSurfaceSyncTracker::recordWaitSubmit(bool submittedWork)
{
    m_submittedSincePresent |= submittedWork;
}

void PlumeSurfaceSyncTracker::recordSurfaceUpload(bool uploaded,
                                                  uint32_t resourceId)
{
    m_submittedSincePresent |= uploaded;
    if (uploaded && resourceId)
        m_uploadedSurfaces.insert(resourceId);
}

void PlumeSurfaceSyncTracker::recordHostFrameRetired(bool retired)
{
    /* Retiring a sticky host frame changes the visible source even when the
     * guest's replacement surface was rendered before the overlay started.
     * Keep one present pending until that surface has reached the swapchain. */
    m_submittedSincePresent |= retired;
}

bool PlumeSurfaceSyncTracker::needsPresent(bool hasQueuedWork,
                                           bool needsStickyFrame) const
{
    return hasQueuedWork || needsStickyFrame || m_submittedSincePresent;
}

bool PlumeSurfaceSyncTracker::hasUploadedSurface(uint32_t resourceId) const
{
    return resourceId &&
           m_uploadedSurfaces.find(resourceId) != m_uploadedSurfaces.end();
}

void PlumeSurfaceSyncTracker::recordPresent()
{
    m_submittedSincePresent = false;
    m_uploadedSurfaces.clear();
}

using ColorKey = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint32_t, uint32_t>;
using ZetaKey = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t>;
using FramebufferKey = std::tuple<uint64_t, uint64_t, uint32_t>;

struct PlumeSurfaceBindingTracker::Impl {
    uint64_t next_generation = 1;
    std::map<ColorKey, uint64_t> colors;
    std::map<ZetaKey, uint64_t> zetas;
    std::map<FramebufferKey, uint64_t> framebuffers;

    template <typename Key>
    uint64_t findOrCreate(std::map<Key, uint64_t> &entries, const Key &key)
    {
        auto found = entries.find(key);
        if (found != entries.end())
            return found->second;
        uint64_t generation = next_generation++;
        entries.emplace(key, generation);
        return generation;
    }
};

PlumeSurfaceBindingTracker::PlumeSurfaceBindingTracker()
    : m_impl(std::make_unique<Impl>())
{
}

PlumeSurfaceBindingTracker::~PlumeSurfaceBindingTracker() = default;
PlumeSurfaceBindingTracker::PlumeSurfaceBindingTracker(
    PlumeSurfaceBindingTracker &&) noexcept = default;
PlumeSurfaceBindingTracker &PlumeSurfaceBindingTracker::operator=(
    PlumeSurfaceBindingTracker &&) noexcept = default;

PlumeSurfaceBindingIds PlumeSurfaceBindingTracker::bind(
    const XgpuSurfaceBinding &binding)
{
    PlumeSurfaceBindingIds ids;
    ColorKey colorKey(binding.color_resource, binding.width, binding.height,
                      binding.color_pitch, binding.color_format, binding.layout,
                      binding.sample_count);
    ids.color_generation = m_impl->findOrCreate(m_impl->colors, colorKey);

    if (binding.zeta_resource && binding.zeta_format != XGPU_ZETA_NONE) {
        const uint32_t zetaWidth = binding.zeta_width
            ? binding.zeta_width
            : (binding.image_width ? binding.image_width : binding.width);
        const uint32_t zetaHeight = binding.zeta_height
            ? binding.zeta_height
            : (binding.image_height ? binding.image_height : binding.height);
        /* A pitch and a swizzled colour target can share one guest zeta
         * allocation. Key depth by its own layout; using the colour layout
         * splits that depth image into two host generations and makes
         * address-based resolves select whichever colour target bound last. */
        ZetaKey zetaKey(binding.zeta_resource, zetaWidth, zetaHeight,
                        binding.zeta_pitch, binding.zeta_format,
                        binding.zeta_layout, binding.sample_count,
                        binding.zeta_float);
        ids.zeta_generation = m_impl->findOrCreate(m_impl->zetas, zetaKey);
    }

    FramebufferKey framebufferKey(ids.color_generation, ids.zeta_generation,
                                  binding.sample_count);
    ids.framebuffer_generation = m_impl->findOrCreate(
        m_impl->framebuffers, framebufferKey);
    return ids;
}

} // namespace xgpu::plume
