from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPILER_HEADER = (
    ROOT / "hw/xbox/d3d_hle/plume/plume_shader_compiler.h"
).read_text()
COMPILER = (
    ROOT / "hw/xbox/d3d_hle/plume/plume_shader_compiler.cpp"
).read_text()
DRAW_HEADER = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.h").read_text()
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
BACKEND = (ROOT / "hw/xbox/d3d_hle/plume/plume_backend.cpp").read_text()
XBOX_BUILD = (ROOT / "hw/xbox/meson.build").read_text()

assert "std::future<ShaderCompileResult> compileShaderAsync(" in COMPILER_HEADER
assert "void waitForShaderCompiles();" in COMPILER_HEADER
assert "class ShaderCompilerQueue" in COMPILER
assert "std::deque<ShaderCompileJob> m_jobs;" in COMPILER
assert "std::thread m_worker;" in COMPILER
assert "return shaderCompilerQueue().submit(request);" in COMPILER
assert "return m_jobs.empty() && !m_busy;" in COMPILER
assert "shaderCompilerQueue().wait();" in COMPILER
assert "xgpu::plume::waitForShaderCompiles();" in BACKEND
assert "XEMU_D3D_HLE_NO_DXC_LIBRARY" not in XBOX_BUILD
assert "SearchPathW(" in COMPILER
assert "CREATE_NO_WINDOW" in COMPILER
assert "-DXRECOMP_DXC_EMBEDDED=1" in XBOX_BUILD
assert "plume_embedded_dxc_win32.cpp" in XBOX_BUILD
assert "xrecomp_dxc_resource" in XBOX_BUILD

assert DRAW_HEADER.count(
    "std::future<ShaderCompileResult> compileFuture;"
) == 3
assert "future.wait_for(std::chrono::seconds(0))" in DRAW

pixel_start = DRAW.index("::plume::RenderShader *PlumeDraw::progPixelShader(")
pixel_end = DRAW.index("void PlumeDraw::pollPixelShaderOverrides", pixel_start)
pixel = DRAW[pixel_start:pixel_end]
assert "queueShaderCompile(" in pixel
assert "shaderCompileReady(ps.compileFuture)" in pixel
assert "compileForContext(" not in pixel

vertex_start = DRAW.index("::plume::RenderShader *PlumeDraw::progVertexShader(")
vertex_end = DRAW.index("::plume::RenderPipeline *PlumeDraw::progPso(", vertex_start)
vertex = DRAW[vertex_start:vertex_end]
assert "queueShaderCompile(" in vertex
assert "shaderCompileReady(vs.compileFuture)" in vertex
assert "compileForContext(" not in vertex

program_start = DRAW.index("int PlumeDraw::setVertexProgram(")
program_end = DRAW.index("void PlumeDraw::consumeVertexPrograms", program_start)
program = DRAW[program_start:program_end]
assert "queueShaderCompile(" in program
assert "shaderCompileReady(guestProgram.compileFuture)" in program
assert "compileForTarget(" not in program

create_start = DRAW.index("uint32_t PlumeDraw::createPixelShader(")
create_end = DRAW.index("void PlumeDraw::setActivePS", create_start)
assert "queueShaderCompile(" in DRAW[create_start:create_end]

register_start = DRAW.index("bool PlumeDraw::registerVertexShader(")
register_end = DRAW.index("void PlumeDraw::setActiveVertexShader", register_start)
assert "queueShaderCompile(" in DRAW[register_start:register_end]

print("d3d_hle_async_shader_compile_contract: OK")
