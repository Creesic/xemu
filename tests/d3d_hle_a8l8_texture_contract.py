"""Contract: A8/A8L8 light cubes bind their authored alpha content."""

import re
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] /
          "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text(
              encoding="utf-8", errors="replace")

assert "case 0x1A: out = RenderFormat::R8G8_UNORM" in SOURCE
assert "case 0x20: out = RenderFormat::R8G8_UNORM" in SOURCE
assert "format != 0x19u && format != 0x1Au" in SOURCE
assert len(re.findall(
    r"RenderSwizzle::ONE,\s*RenderSwizzle::ONE,\s*"
    r"RenderSwizzle::ONE,\s*RenderSwizzle::R", SOURCE)) >= 2
assert len(re.findall(
    r"RenderSwizzle::R,\s*RenderSwizzle::R,\s*"
    r"RenderSwizzle::R,\s*RenderSwizzle::G", SOURCE)) >= 2
assert "m_whiteCubeTex" in SOURCE and "m_whiteCubeView" in SOURCE
assert "cubeTextureMask & (1u << s)" in SOURCE

print("d3d_hle_a8l8_texture_contract: OK")
