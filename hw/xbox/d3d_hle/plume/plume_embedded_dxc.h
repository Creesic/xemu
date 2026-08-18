#ifndef XGPU_PLUME_EMBEDDED_DXC_H
#define XGPU_PLUME_EMBEDDED_DXC_H

#include <filesystem>
#include <string>

namespace xgpu {
namespace plume {

/* Extracts the DXC payload embedded in the current executable into a
 * content-versioned per-user cache. Existing files are accepted only after
 * their size and SHA-256 match the embedded manifest. */
bool ensureEmbeddedDxc(std::filesystem::path &executable,
                       std::string &diagnostics);

} /* namespace plume */
} /* namespace xgpu */

#endif
