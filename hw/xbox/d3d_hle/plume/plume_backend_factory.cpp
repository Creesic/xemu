#include "plume_backend_factory.h"

#include <algorithm>
#include <cctype>
#include <string>

#ifndef XGPU_BACKEND_FACTORY_TEST_ONLY
namespace plume {
#if defined(PLUME_D3D12_ENABLED)
std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
#if defined(PLUME_VULKAN_ENABLED) && defined(PLUME_SDL_VULKAN_ENABLED)
std::unique_ptr<RenderInterface> CreateVulkanInterface(RenderWindow window);
#elif defined(PLUME_VULKAN_ENABLED)
std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
#if defined(PLUME_METAL_ENABLED)
std::unique_ptr<RenderInterface> CreateMetalInterface();
#endif
} /* namespace plume */
#endif

namespace xgpu {
namespace plume {

bool parsePlumeBackend(const char *text, PlumeBackend &backend)
{
    std::string value = text ? text : "auto";
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (value.empty() || value == "auto")
        backend = PlumeBackend::AUTO;
    else if (value == "d3d12")
        backend = PlumeBackend::D3D12;
    else if (value == "vulkan")
        backend = PlumeBackend::VULKAN;
    else if (value == "metal")
        backend = PlumeBackend::METAL;
    else
        return false;

    return true;
}

PlumeBackend resolvePlumeBackend(PlumeBackend requested)
{
    if (requested != PlumeBackend::AUTO)
        return requested;

#if defined(PLUME_D3D12_ENABLED)
    return PlumeBackend::D3D12;
#elif defined(PLUME_METAL_ENABLED)
    return PlumeBackend::METAL;
#elif defined(PLUME_VULKAN_ENABLED)
    return PlumeBackend::VULKAN;
#else
    return PlumeBackend::AUTO;
#endif
}

const char *plumeBackendName(PlumeBackend backend)
{
    switch (backend) {
    case PlumeBackend::AUTO:
        return "auto";
    case PlumeBackend::D3D12:
        return "d3d12";
    case PlumeBackend::VULKAN:
        return "vulkan";
    case PlumeBackend::METAL:
        return "metal";
    }

    return "unknown";
}

#ifndef XGPU_BACKEND_FACTORY_TEST_ONLY

namespace host {

#if defined(PLUME_D3D12_ENABLED)
std::unique_ptr<::plume::RenderInterface> createD3D12()
{
    return ::plume::CreateD3D12Interface();
}
#endif

#if defined(PLUME_VULKAN_ENABLED)
std::unique_ptr<::plume::RenderInterface> createVulkan(
    const XgpuNativeWindow &window, std::string &error)
{
#if defined(PLUME_SDL_VULKAN_ENABLED)
    ::plume::RenderWindow renderWindow = makePlumeWindow(window, error);
    if (!error.empty())
        return nullptr;
    return ::plume::CreateVulkanInterface(renderWindow);
#else
    (void)window;
    (void)error;
    return ::plume::CreateVulkanInterface();
#endif
}
#endif

#if defined(PLUME_METAL_ENABLED)
std::unique_ptr<::plume::RenderInterface> createMetal()
{
    return ::plume::CreateMetalInterface();
}
#endif

} /* namespace host */

std::unique_ptr<::plume::RenderInterface> createPlumeInterface(
    PlumeBackend backend, const XgpuNativeWindow &window, std::string &error)
{
    backend = resolvePlumeBackend(backend);
    error.clear();
#if !defined(PLUME_VULKAN_ENABLED)
    (void)window;
#endif

    switch (backend) {
    case PlumeBackend::D3D12:
#if defined(PLUME_D3D12_ENABLED)
        return host::createD3D12();
#else
        error = "D3D12 was not built for this executable";
        return nullptr;
#endif
    case PlumeBackend::VULKAN:
#if defined(PLUME_VULKAN_ENABLED)
        return host::createVulkan(window, error);
#else
        error = "Vulkan was not built for this executable";
        return nullptr;
#endif
    case PlumeBackend::METAL:
#if defined(PLUME_METAL_ENABLED)
        return host::createMetal();
#else
        error = "Metal was not built for this executable";
        return nullptr;
#endif
    case PlumeBackend::AUTO:
        break;
    }

    error = "Unable to resolve the requested Plume backend";
    return nullptr;
}

::plume::RenderWindow makePlumeWindow(const XgpuNativeWindow &window,
                                      std::string &error)
{
    error.clear();

#if defined(_WIN32)
    if (window.kind != XGPU_NATIVE_WINDOW_WIN32 || window.window == 0) {
        error = "Plume requires a valid Win32 window";
        return nullptr;
    }
    return reinterpret_cast<::plume::RenderWindow>(window.window);
#elif defined(PLUME_SDL_VULKAN_ENABLED)
    if (window.kind != XGPU_NATIVE_WINDOW_SDL || window.view == nullptr) {
        error = "Plume requires a valid SDL window";
        return nullptr;
    }
    return static_cast<::plume::RenderWindow>(window.view);
#elif defined(__linux__)
    if (window.kind != XGPU_NATIVE_WINDOW_X11 || window.display == nullptr ||
        window.window == 0) {
        error = "Plume requires a valid X11 display and window";
        return {};
    }
    return { static_cast<Display *>(window.display),
             static_cast<Window>(window.window) };
#elif defined(__APPLE__)
    if (window.kind != XGPU_NATIVE_WINDOW_APPLE || window.window == 0 ||
        window.view == nullptr) {
        error = "Plume requires valid Apple window and view objects";
        return {};
    }
    return { reinterpret_cast<void *>(window.window), window.view };
#else
    (void)window;
    error = "Plume native windows are unsupported on this platform";
    return {};
#endif
}

#endif /* XGPU_BACKEND_FACTORY_TEST_ONLY */

} /* namespace plume */
} /* namespace xgpu */
