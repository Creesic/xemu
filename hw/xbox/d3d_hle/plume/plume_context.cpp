/*
 * plume_context.cpp — Platform-neutral Plume host-device lifecycle.
 */
#include "plume_context.h"
#include "plume_perf.h"
#include "plume_wait_stats.h"

#include <cstdio>
#include <string>

namespace xgpu {
namespace plume {

bool PlumeContext::init(const Desc &desc)
{
    if (m_inited)
        return true;
    if (m_failed)
        return false;

    std::string error;
    const ::plume::RenderWindow renderWindow = makePlumeWindow(desc.window, error);
    if (!error.empty()) {
        fprintf(stderr, "[PLUME] context: %s\n", error.c_str());
        m_failed = true;
        return false;
    }

    const PlumeBackend backend = resolvePlumeBackend(desc.backend);
    m_iface = createPlumeInterface(backend, desc.window, error);
    if (!m_iface) {
        fprintf(stderr, "[PLUME] context: %s backend creation failed%s%s\n",
                plumeBackendName(backend), error.empty() ? "" : ": ",
                error.c_str());
        m_failed = true;
        return false;
    }

    m_device = m_iface->createDevice();
    if (!m_device) {
        fprintf(stderr, "[PLUME] context: createDevice failed\n");
        m_failed = true;
        return false;
    }

    m_queue = m_device->createCommandQueue(::plume::RenderCommandListType::DIRECT);
    if (!m_queue) {
        fprintf(stderr, "[PLUME] context: command queue creation failed\n");
        m_failed = true;
        return false;
    }

    m_fence = m_device->createCommandFence();
    m_presentFence = m_device->createCommandFence();
    if (!m_fence || !m_presentFence) {
        fprintf(stderr, "[PLUME] context: command fence creation failed\n");
        m_failed = true;
        return false;
    }

    m_swapChain = m_queue->createSwapChain(
        ::plume::RenderSwapChainDesc(renderWindow,
                                     ::plume::RenderFormat::B8G8R8A8_UNORM, 2));
    if (!m_swapChain) {
        fprintf(stderr, "[PLUME] context: swap chain creation failed\n");
        m_failed = true;
        return false;
    }

    m_swapChain->resize();
    m_cmdList = m_queue->createCommandList();
    m_uploadCmd = m_queue->createCommandList();
    m_acquireSem = m_device->createCommandSemaphore();

    if (!m_cmdList || !m_uploadCmd || !m_acquireSem) {
        fprintf(stderr, "[PLUME] context: RHI object creation failed\n");
        m_failed = true;
        return false;
    }

    rebuildFramebuffers();

    m_inited = true;
    fprintf(stderr, "[PLUME] context: %s device up (%ux%u, %u swap images)\n",
            plumeBackendName(backend),
            m_swapChain->getWidth(), m_swapChain->getHeight(),
            m_swapChain->getTextureCount());
    fflush(stderr);
    return true;
}

bool PlumeContext::acquire(uint32_t *outSwapIndex)
{
    if (!m_inited || !outSwapIndex)
        return false;

    uint32_t idx = 0;
    if (!m_swapChain->acquireTexture(m_acquireSem.get(), &idx)) {
        m_swapChain->resize();
        rebuildFramebuffers();
        return false;
    }

    *outSwapIndex = idx;
    return true;
}

void PlumeContext::rebuildFramebuffers()
{
    m_framebuffers.clear();
    if (!m_device || !m_swapChain)
        return;

    for (uint32_t i = 0; i < m_swapChain->getTextureCount(); i++) {
        const ::plume::RenderTexture *color = m_swapChain->getTexture(i);
        ::plume::RenderFramebufferDesc fbd;
        fbd.colorAttachments = &color;
        fbd.colorAttachmentsCount = 1;
        fbd.depthAttachment = nullptr;
        m_framebuffers.push_back(m_device->createFramebuffer(fbd));
    }
}

void PlumeContext::submitAndPresent(uint32_t swapIndex)
{
    if (!m_inited)
        return;

    while (m_releaseSems.size() < m_swapChain->getTextureCount())
        m_releaseSems.emplace_back(m_device->createCommandSemaphore());

    const ::plume::RenderCommandList *cl = m_cmdList.get();
    ::plume::RenderCommandSemaphore *waitSem = m_acquireSem.get();
    ::plume::RenderCommandSemaphore *sigSem = m_releaseSems[swapIndex].get();
    uint64_t perf_submit_t0 = xgpu_plume_perf_begin();
    m_queue->executeCommandLists(&cl, 1, &waitSem, 1, &sigSem, 1, m_fence.get());
    xgpu_plume_perf_end(XGPU_PLUME_PERF_SUBMIT, perf_submit_t0);
    uint64_t perf_present_t0 = xgpu_plume_perf_begin();
    uint64_t wait_swap_t0 = xgpu_plume_wait_stats_begin();
    m_swapChain->present(swapIndex, &sigSem, 1);
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_PRESENT_SWAP, wait_swap_t0);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_PRESENT, perf_present_t0);
    uint64_t perf_fence_t0 = xgpu_plume_perf_begin();
    uint64_t wait_fence_t0 = xgpu_plume_wait_stats_begin();
    m_queue->waitForCommandFence(m_fence.get());
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_PRESENT_FENCE, wait_fence_t0);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_FENCE, perf_fence_t0);
    xgpu_plume_wait_stats_present();
}

void PlumeContext::submitAndPresentDeferred(uint32_t swapIndex)
{
    if (!m_inited)
        return;
    /* One deferred present maximum: the previous one must be retired before
     * this submission can reuse the command list and fence. */
    waitPendingPresent();

    while (m_releaseSems.size() < m_swapChain->getTextureCount())
        m_releaseSems.emplace_back(m_device->createCommandSemaphore());

    const ::plume::RenderCommandList *cl = m_cmdList.get();
    ::plume::RenderCommandSemaphore *waitSem = m_acquireSem.get();
    ::plume::RenderCommandSemaphore *sigSem = m_releaseSems[swapIndex].get();
    uint64_t perf_submit_t0 = xgpu_plume_perf_begin();
    m_queue->executeCommandLists(&cl, 1, &waitSem, 1, &sigSem, 1,
                                 m_presentFence.get());
    xgpu_plume_perf_end(XGPU_PLUME_PERF_SUBMIT, perf_submit_t0);
    uint64_t perf_present_t0 = xgpu_plume_perf_begin();
    uint64_t wait_swap_t0 = xgpu_plume_wait_stats_begin();
    m_swapChain->present(swapIndex, &sigSem, 1);
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_PRESENT_SWAP, wait_swap_t0);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_PRESENT, perf_present_t0);
    m_presentInFlight = true;
    xgpu_plume_wait_stats_present();
}

void PlumeContext::waitPendingPresent()
{
    if (!m_presentInFlight)
        return;
    uint64_t perf_fence_t0 = xgpu_plume_perf_begin();
    uint64_t wait_fence_t0 = xgpu_plume_wait_stats_begin();
    m_queue->waitForCommandFence(m_presentFence.get());
    xgpu_plume_wait_stats_end(XGPU_PLUME_WAIT_PRESENT_FENCE, wait_fence_t0);
    xgpu_plume_perf_end(XGPU_PLUME_PERF_FENCE, perf_fence_t0);
    m_presentInFlight = false;
}

::plume::RenderFramebuffer *PlumeContext::framebuffer(uint32_t index)
{
    return index < m_framebuffers.size() ? m_framebuffers[index].get() : nullptr;
}

::plume::RenderCommandSemaphore *PlumeContext::releaseSem(uint32_t index)
{
    while (m_releaseSems.size() <= index)
        m_releaseSems.emplace_back(m_device->createCommandSemaphore());
    return m_releaseSems[index].get();
}

uint32_t PlumeContext::width() const
{
    return m_swapChain ? m_swapChain->getWidth() : 0;
}

uint32_t PlumeContext::height() const
{
    return m_swapChain ? m_swapChain->getHeight() : 0;
}

} /* namespace plume */
} /* namespace xgpu */
