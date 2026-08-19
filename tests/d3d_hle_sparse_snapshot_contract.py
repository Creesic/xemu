from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()

assert "discovery_sparse_alloc" in SOURCE
assert "discovery_sparse_commit" in SOURCE
assert "discovery_sparse_free" in SOURCE
assert "VirtualAlloc" in SOURCE or "mmap" in SOURCE
assert "g_malloc0(image_end)" not in SOURCE

start = SOURCE.index("const XemuD3DHleProfile *xemu_d3d_hle_discover")
end = SOURCE.index("static uint32_t register_value", start)
body = SOURCE[start:end]
assert "discovery_sparse_commit" in body
assert "discovery_sparse_free" in body

print("d3d_hle_sparse_snapshot_contract: OK")
