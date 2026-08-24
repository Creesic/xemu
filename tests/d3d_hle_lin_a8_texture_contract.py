"""Contract: LIN_A8 (0x1F) must upload like A8 (0x19), not fall back to white.

Oracle: XDK pixeljar.hpp g_TextureFormat — 0x1F D3DFMT_LIN_A8 is FMT_8BPP |
FMT_LINEAR; d3d8types.h names it D3DFMT_LIN_A8. Identical texel semantics to
swizzled A8 (alpha-only, RGB read as 1.0), stored row-major with a pitch —
strictly SIMPLER to upload than swizzled A8, which is already supported.

Runtime evidence: RSC2 (build 5849) creates a 192x125 LIN_A8 texture with
row pitch 192 immediately after device creation (non-pow2 dimensions are only
legal for linear formats, so this cannot be satisfied by the swizzled path).
An unmapped format makes every draw sampling it use the white fallback.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / \
    "hw/xbox/d3d_hle/plume/plume_draw.cpp"


def extract_function(text, name):
    start = text.index(f"{name}(")
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise AssertionError(f"unbalanced braces in {name}")


def main():
    text = SRC.read_text(encoding="utf-8", errors="replace")
    body = extract_function(text, "static bool plume_map_texfmt")
    m = re.search(r"case\s+0x1F[uU]?\s*:\s*out\s*=\s*RenderFormat::R8_UNORM",
                  body)
    assert m, (
        "plume_map_texfmt must map 0x1F (D3DFMT_LIN_A8) to R8_UNORM; "
        "unmapped it silently samples the white fallback (RSC2 creates a "
        "192x125 pitch-192 LIN_A8 texture at boot)"
    )
    # The alpha-replication view swizzle (RGB=1, A=R) must cover LIN_A8
    # wherever it covers A8.
    swizzle_sites = [
        m.start() for m in re.finditer(r"format\s*==\s*0x19[uU]?", text)
    ]
    assert swizzle_sites, "expected an A8 component-mapping special case"
    for site in swizzle_sites:
        window = text[site : site + 200]
        assert re.search(r"0x1F[uU]?", window), (
            "A8 special-case near offset %d does not also cover LIN_A8 "
            "(0x1F); both formats need RGB=ONE, A=R component mapping"
            % site
        )
    print("d3d_hle_lin_a8_texture_contract: OK")


if __name__ == "__main__":
    sys.exit(main())
