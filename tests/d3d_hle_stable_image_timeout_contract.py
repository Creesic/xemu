from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "s_header_valid_ms" in SOURCE
assert "s_discovery_scanned_epoch" in SOURCE
assert "coverage_epoch" in SOURCE
assert "D3D section never fully mapped" in SOURCE
assert "s_discovery_retry_attempts" not in SOURCE
assert "s_discovery_retry_at_ms" not in SOURCE

print("d3d_hle_stable_image_timeout_contract: OK")
