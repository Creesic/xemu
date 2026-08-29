"""Contract: reusable guest shader handles record immutable shader IDs."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.h").read_text()


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
activate = function("void PlumeDraw::setActiveVertexShader(")
record = function("void PlumeDraw::recordDraw(")

assert "m_vsByKey" in HEADER
assert "m_vsHandleMap" in HEADER
assert "uint32_t m_vsNext = 1;" in HEADER
assert "auto duplicate = m_vsByKey.find(key);" in register
assert "m_vsHandleMap[handle] = duplicate->second;" in register
assert "const uint32_t stableHandle = m_vsNext++;" in register
assert "m_vsReg.emplace(stableHandle, std::move(shader));" in register
assert "m_vsByKey.emplace(std::move(key), stableHandle);" in register
assert "m_vsHandleMap[handle] = stableHandle;" in register
assert "m_vsReg[handle]" not in register
assert "m_progPsos.clear()" not in register
assert "m_vsHandleMap.find(handle)" in activate
assert "m_activeVS = shader != m_vsHandleMap.end() ? shader->second : 0;" in activate
assert "const uint32_t vsH = m_vsReg.find(m_activeVS)" in record

print("plume_vertex_shader_registration_lifetime_contract: OK")
