"""Contract: Begin/End preserves the active programmable vertex shader.

Xbox immediate mode supplies v0-v15 float4 latches directly; it does not
reinterpret those registers through the shader object's stream declaration.
FM1's press-start quad writes position in v0, UV in v3, and color in v9.
Treating that draw as fixed FVF relabelled the inputs and viewport-transformed
already-screen-space output a second time.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
DEVICE = (ROOT / "hw/xbox/d3d_hle/d3d8_device.c").read_text()
VSH = (ROOT / "hw/xbox/d3d_hle/d3d8_vsh.c").read_text()


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    raise AssertionError(f"unbalanced function: {signature}")


end = function(GUEST, "void d3d_hle_guest_end(void)")
programmable = end.index("d3d8_vsh_is_programmable(host_shader)")
fixed_mask = end.index("Begin/End unsupported attribute mask")
assert "GetVertexShader(device, &host_shader)" in end
assert programmable < fixed_mask
assert "d3d8_DrawImmediate(" in end
assert "g_hle_imm_verts, sizeof(g_hle_imm_verts[0])" in end
assert "g_hle_imm_verts[v][0][3] != 1.0f" not in end

submit = function(DEVICE, "static HRESULT submit_draw(")
assert "prepare_draw(immediate)" in submit
assert "d3d8_vsh_calculate_immediate_position_bounds(" in submit

variant = function(VSH, "static BOOL vsh_prepare_draw_variant(")
assert "plume_declaration.attributes_present = program->inputs_read" in variant
assert "plume_declaration.format[i] = 0x42u" in variant
assert "i * 4u * sizeof(float)" in variant
assert "NV2A_VS_IMMEDIATE_HANDLE_BASE" in variant
assert "d3d8_vsh_generate_hlsl(\n            program, vertex_format_ptr" in variant

color = function(GUEST, "void d3d_hle_guest_set_vertex_data_color(")
data4ub = function(GUEST, "void d3d_hle_guest_set_vertex_data4ub(")
assert "(color >> 16) & 0xFFu" in color
assert "(color >> 8) & 0xFFu" in color
assert "color & 0xFFu" in color
assert "(color >> 24) & 0xFFu" in color
assert "d3d_hle_guest_set_vertex_data4ub(" in color
assert "d3d_hle_guest_set_vertex_data_color" not in data4ub
assert "components[0] = (float)(a & 0xFFu) / 255.0f" in data4ub
assert "components[3] = (float)(d & 0xFFu) / 255.0f" in data4ub

print("d3d_hle_immediate_programmable_contract: OK")
