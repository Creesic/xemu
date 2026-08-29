"""Contract: Xbox three-component vertex formats are host-repacked."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
VSH = (ROOT / "hw/xbox/d3d_hle/d3d8_vsh.c").read_text()


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


guest_size = function("static uint32_t nv2a_attr_guest_size(")
layout = function("static bool nv2a_build_vertex_repack_layout(")
repack = function("static bool nv2a_repack_vertex_array(")
pso = function("::plume::RenderPipeline *PlumeDraw::progPso(")
record = function("void PlumeDraw::recordDraw(")
cached = function("bool PlumeDraw::recordCachedIndexedDraw(")
indexed = function("XgpuPlumeGpuDrawResult PlumeDraw::recordProgIndexedDraw(")

assert "case 0x31u" in guest_size and "case 0x35u" in guest_size
assert "return 6;" in guest_size
assert "case 0x34u" in guest_size and "return 3;" in guest_size
assert "guestSize != hostSize" in layout
assert "result.mask |= (uint16_t)(1u << attr);" in layout
assert "destination.assign((size_t)stagedBytes, 0u);" in repack
assert "guest + guestOffset[attr]" in repack
assert "nv2a_attr_guest_size(format[attr])" in repack
assert "repack.offset[input]" in pso
assert record.index("nv2a_repack_vertex_array(") < record.index(
    "materializeLatchedVertexArray("
)
assert "cachedVertices" in cached and "mesh.stride = cachedStride;" in cached
assert "d.attrOffset[i] = repack.mask" in indexed
assert "over-reads a 4th short" not in VSH

print("d3d_hle_vertex_format_repack_contract: OK")
