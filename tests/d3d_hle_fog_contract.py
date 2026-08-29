"""Contract: programmable fog matches the NV2A shader interface."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
VSH = (ROOT / "hw/xbox/d3d_hle/d3d8_vsh.c").read_text()
PSH = (ROOT / "hw/xbox/d3d_hle/d3d8_combiners.c").read_text()
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()

fog_masks = re.search(
    r"static const uint8_t fog_mask\[16\] = \{(.*?)\};", VSH, re.S
)
assert fog_masks
assert [int(value, 16) for value in re.findall(r"0x([0-9A-F]+)", fog_masks[1])] == [
    0x0, 0x8, 0x8, 0xC, 0x8, 0xC, 0xC, 0xE,
    0x8, 0xC, 0xC, 0xE, 0xC, 0xE, 0xE, 0xF,
]
assert VSH.count("vsh_output_write_mask(") == 3

assert 'EMIT("    float  fog     : FOG;\\n");' in PSH
assert "float xrecomp_fog_factor(float w, float vertex_fog)" in PSH
assert "if (fog_mode == 0u) return saturate(vertex_fog);" in PSH
assert "fog_enable != 0u ? xrecomp_fog_factor(input.pos.w, input.fog)" in PSH
assert "if (!final_uses_fog)" in PSH

assert "float fog:FOG;" in DRAW
assert DRAW.count("o.fog=col1.a;") == 4

print("d3d_hle_fog_contract: OK")
