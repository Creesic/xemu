#ifndef XGPU_PLUME_SHADER_COMPILER_H
#define XGPU_PLUME_SHADER_COMPILER_H

#include "plume_render_interface_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xgpu {
namespace plume {

enum class ShaderTarget {
    DXIL,
    SPIRV,
    METAL_SOURCE,
};

struct ShaderCompileRequest {
    const char *source = nullptr;
    const char *entryPoint = nullptr;
    const char *profile = nullptr;
    ShaderTarget target = ShaderTarget::DXIL;
};

struct ShaderCompileResult {
    bool ok = false;
    ShaderTarget target = ShaderTarget::DXIL;
    std::vector<uint8_t> bytecode;
    std::string entryPoint;
    std::string diagnostics;
};

std::vector<std::string> buildDxcArguments(
    const ShaderCompileRequest &request,
    const std::string &inputPath,
    const std::string &outputPath);
ShaderCompileResult compileShader(const ShaderCompileRequest &request);

ShaderTarget shaderTargetForRenderFormat(::plume::RenderShaderFormat format);
::plume::RenderShaderFormat renderShaderFormatForTarget(ShaderTarget target);
const char *shaderTargetName(ShaderTarget target);

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_SHADER_COMPILER_H */
