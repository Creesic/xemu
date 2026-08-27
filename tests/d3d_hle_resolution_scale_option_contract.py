#!/usr/bin/env python3
"""Contract for Xemu's shared internal-resolution option in Plume mode."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    match = re.search(
        rf"(?m)^[^\n;{{}}]*\b{re.escape(name)}\s*\([^;{{}}]*\)\s*{{",
        text,
    )
    assert match, f"function definition not found: {name}"
    brace = text.index("{", match.start())
    depth = 0
    for index in range(brace, len(text)):
        depth += text[index] == "{"
        depth -= text[index] == "}"
        if depth == 0:
            return text[match.start() : index + 1]
    raise AssertionError(f"unbalanced function: {name}")


def main():
    pgraph = (ROOT / "hw/xbox/nv2a/pgraph/pgraph.c").read_text()
    hle = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()
    backend = (ROOT / "hw/xbox/d3d_hle/plume/plume_backend.cpp").read_text()
    scale = (ROOT / "hw/xbox/d3d_hle/plume/plume_resolution_scale.h").read_text()
    d3d12 = (ROOT / "thirdparty/plume/plume_d3d12.cpp").read_text()

    setter = function(pgraph, "nv2a_set_surface_scale_factor")
    getter = function(pgraph, "nv2a_get_surface_scale_factor")
    assert "xemu_d3d_hle_owns_window()" in setter
    assert "xemu_d3d_hle_set_surface_scale_factor(scale)" in setter
    assert "xemu_d3d_hle_owns_window()" in getter
    assert "xemu_d3d_hle_get_surface_scale_factor()" in getter
    assert setter.index("xemu_d3d_hle_set_surface_scale_factor(scale)") < setter.index(
        "bql_unlock();"
    )
    assert getter.index("xemu_d3d_hle_get_surface_scale_factor()") < getter.index(
        "bql_unlock();"
    )

    activate = function(hle, "xemu_d3d_hle_activate_host_device")
    reactivate = function(hle, "xemu_d3d_hle_reactivate_host_device")
    hle_setter = function(hle, "xemu_d3d_hle_set_surface_scale_factor")
    hle_cpu_setter = function(
        hle, "xemu_d3d_hle_set_surface_scale_factor_on_cpu"
    )
    assert "xemu_d3d_hle_apply_surface_scale();" in activate
    assert "xemu_d3d_hle_apply_surface_scale();" in reactivate
    assert "run_on_cpu(" in hle_setter
    assert "xgpu_plume_set_internal_resolution_scale" not in hle_setter
    assert "xgpu_plume_set_internal_resolution_scale" in hle_cpu_setter

    plume_setter = function(backend, "xgpu_plume_set_internal_resolution_scale")
    assert "if (!g_draw.ready())" in plume_setter
    assert "kMaxInternalResolutionScale = 10" in scale
    assert d3d12.count("return texture->d3d ? std::move(texture) : nullptr;") == 2
    print("d3d_hle_resolution_scale_option_contract: OK")


if __name__ == "__main__":
    main()
