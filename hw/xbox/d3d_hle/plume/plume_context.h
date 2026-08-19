/*
 * plume_context.h — Plume RHI device / swapchain lifecycle.
 * Native-window creation and event handling remain owned by the host nativeish.
 * Draw replay lives in plume_draw.cpp.
 */
#ifndef XGPU_PLUME_CONTEXT_H
#define XGPU_PLUME_CONTEXT_H

#include "../xgpu_renderer.h"
#include "plume_backend_factory.h"
#include "plume_render_interface.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace xgpu {
namespace plume {

class PlumeContext {
public:
    struct Desc {
        XgpuNativeWindow window = {};
        PlumeBackend backend = PlumeBackend::AUTO;
        uint32_t width = XGPU_PANEL_WIDTH;
        uint32_t height = XGPU_PANEL_HEIGHT;
    };

    PlumeContext() = default;
    ~PlumeContext() = default;

    PlumeContext(const PlumeContext &) = delete;
    PlumeContext &operator=(const PlumeContext &) = delete;

    bool init(const Desc &desc);
    bool ready() const { return m_inited; }
    bool failed() const { return m_failed; }

    bool acquire(uint32_t *outSwapIndex);
    void rebuildFramebuffers();
    void submitAndPresent(uint32_t swapIndex);
    /* Pipelined present: submit and present without blocking, signaling the
     * dedicated present fence. Exactly one present may be deferred; the
     * caller must waitPendingPresent() before the next submit, resize, or
     * any reuse of present-submission resources. */
    void submitAndPresentDeferred(uint32_t swapIndex);
    void waitPendingPresent();
    void reset();
    bool presentInFlight() const { return m_presentInFlight; }

    ::plume::RenderInterface *iface() { return m_iface.get(); }
    const ::plume::RenderInterface *iface() const { return m_iface.get(); }
    ::plume::RenderDevice *device() { return m_device.get(); }
    ::plume::RenderCommandQueue *queue() { return m_queue.get(); }
    ::plume::RenderCommandFence *fence() { return m_fence.get(); }
    ::plume::RenderSwapChain *swapChain() { return m_swapChain.get(); }
    ::plume::RenderCommandList *cmdList() { return m_cmdList.get(); }
    ::plume::RenderCommandList *uploadCmd() { return m_uploadCmd.get(); }
    ::plume::RenderCommandSemaphore *acquireSem() { return m_acquireSem.get(); }
    ::plume::RenderFramebuffer *framebuffer(uint32_t index);
    ::plume::RenderCommandSemaphore *releaseSem(uint32_t index);
    uint32_t width() const;
    uint32_t height() const;

private:
    bool m_inited = false;
    bool m_failed = false;

    std::unique_ptr<::plume::RenderInterface> m_iface;
    std::unique_ptr<::plume::RenderDevice> m_device;
    std::unique_ptr<::plume::RenderCommandQueue> m_queue;
    std::unique_ptr<::plume::RenderCommandFence> m_fence;
    std::unique_ptr<::plume::RenderCommandFence> m_presentFence;
    bool m_presentInFlight = false;
    std::unique_ptr<::plume::RenderSwapChain> m_swapChain;
    std::unique_ptr<::plume::RenderCommandList> m_cmdList;
    std::unique_ptr<::plume::RenderCommandList> m_uploadCmd;
    std::unique_ptr<::plume::RenderCommandSemaphore> m_acquireSem;
    std::vector<std::unique_ptr<::plume::RenderFramebuffer>> m_framebuffers;
    std::vector<std::unique_ptr<::plume::RenderCommandSemaphore>> m_releaseSems;
};

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_CONTEXT_H */
