from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "XemuD3DHleCoverage" in SOURCE
assert "s_coverage" in SOURCE
assert "xemu_d3d_hle_update_coverage" in SOURCE
assert "d3d_needed" in SOURCE
assert "d3d_covered" in SOURCE
assert "page_bits" in SOURCE

entry_start = SOURCE.index("static bool xemu_d3d_hle_is_entry")
entry_end = SOURCE.index("void xemu_d3d_hle_install", entry_start)
entry_body = SOURCE[entry_start:entry_end]
assert "xemu_d3d_hle_update_coverage" in entry_body
coverage_pos = entry_body.index("xemu_d3d_hle_update_coverage")
queue_pos = entry_body.index("xemu_d3d_hle_queue_discovery")
assert coverage_pos < queue_pos
assert "xemu_d3d_hle_resolve_loaded_xbe" not in entry_body
assert "scanning mapped D3D pages" in SOURCE

print("d3d_hle_mapping_coverage_contract: OK")
