"""Contract: registering a new vertex shader cannot destroy in-flight PSOs."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()


def function(signature):
    start = DRAW.index(signature)
    brace = DRAW.index("{", start)
    depth = 0
    for index in range(brace, len(DRAW)):
        if DRAW[index] == "{":
            depth += 1
        elif DRAW[index] == "}":
            depth -= 1
            if depth == 0:
                return DRAW[start : index + 1]
    raise AssertionError(f"unbalanced function: {signature}")


register = function("bool PlumeDraw::registerVertexShader(")
replacement = register.index("if (existing != m_vsReg.end())")
retire_shader = register.index("m_liveRetiredShaders.push_back", replacement)
retire_pipelines = register.index("m_liveRetiredPipelines.push_back", replacement)
clear_pipelines = register.index("m_progPsos.clear()", replacement)
assert replacement < retire_shader < clear_pipelines
assert replacement < retire_pipelines < clear_pipelines
assert register.count("m_progPsos.clear()") == 1

print("plume_vertex_shader_registration_lifetime_contract: OK")
