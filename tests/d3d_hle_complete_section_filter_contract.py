from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()

start = SOURCE.index("const XemuD3DHleProfile *xemu_d3d_hle_discover")
end = SOURCE.index("static uint32_t register_value", start)
body = SOURCE[start:end]

assert "section_copied" in body
assert "XBE_SECTION_HEADER_FLAGS_EXECUTABLE" in SOURCE
assert "discovery_section_is_scan_target" in body
assert "discovery_section_is_named_or_kernel" not in SOURCE
assert "buffer_lower" in body
assert "XbSDBSectionHeader" in body
assert "XbSDB_GenerateSectionFilter" not in body
assert "covered" in body or "copied" in body

print("d3d_hle_complete_section_filter_contract: OK")
