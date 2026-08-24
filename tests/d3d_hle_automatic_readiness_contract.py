from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()

for field in ("has_immediate_begin", "has_immediate_end", "has_render_path"):
    assert field in DISCOVERY

register_start = DISCOVERY.index("static void register_symbol")
register_end = DISCOVERY.index("static int compare_hooks", register_start)
register_body = DISCOVERY[register_start:register_end]
assert '"D3DDevice_Begin"' in register_body
assert '"D3DDevice_End"' in register_body
assert "scan->has_render_path" in register_body

# Lazy/deferred texture state is the bridge for inline XDK setters. Automatic
# Plume must wait/refuse rather than claiming readiness with no materializer.
discover_start = DISCOVERY.index("const XemuD3DHleProfile *xemu_d3d_hle_discover")
discover_end = DISCOVERY.index("static uint32_t register_value", discover_start)
discover_body = DISCOVERY[discover_start:discover_end]
assert "deferred-state bridge" in discover_body
assert "scan.deferred_texture_state_va" in discover_body
assert "dirty_flags_va" in discover_body
assert "has_render_path" in discover_body

lazy_start = DISCOVERY.index("static bool add_lazy_set_state")
lazy_end = DISCOVERY.index("static void set_error", lazy_start)
lazy_body = DISCOVERY[lazy_start:lazy_end]
assert "XBE_SECTION_HEADER_FLAGS_EXECUTABLE" in lazy_body
assert "XBE_SECTION_HEADER_FLAGS_PRELOAD" not in lazy_body

print("d3d_hle_automatic_readiness_contract: OK")
