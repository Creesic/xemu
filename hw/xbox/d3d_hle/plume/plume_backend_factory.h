#ifndef XGPU_PLUME_BACKEND_FACTORY_H
#define XGPU_PLUME_BACKEND_FACTORY_H

#include "../xgpu_native_window.h"
#include "plume_render_interface.h"

#include <memory>
#include <string>

namespace xgpu {
namespace plume {

enum class PlumeBackend {
    AUTO,
    D3D12,
    VULKAN,
    METAL,
};

bool parsePlumeBackend(const char *text, PlumeBackend &backend);
PlumeBackend resolvePlumeBackend(PlumeBackend requested);
const char *plumeBackendName(PlumeBackend backend);

#ifndef XGPU_BACKEND_FACTORY_TEST_ONLY
std::unique_ptr<::plume::RenderInterface> createPlumeInterface(
    PlumeBackend backend, const XgpuNativeWindow &window, std::string &error);
::plume::RenderWindow makePlumeWindow(const XgpuNativeWindow &window,
                                      std::string &error);
#endif

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_BACKEND_FACTORY_H */
