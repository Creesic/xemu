from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "s_discovery_job_queued" in SOURCE
assert "xemu_d3d_hle_discovery_on_cpu" in SOURCE
assert "xemu_d3d_hle_queue_discovery" in SOURCE
assert "async_run_on_cpu" in SOURCE

entry_start = SOURCE.index("static bool xemu_d3d_hle_is_entry")
entry_end = SOURCE.index("void xemu_d3d_hle_install", entry_start)
entry_body = SOURCE[entry_start:entry_end]
assert "xemu_d3d_hle_queue_discovery" in entry_body
assert "xemu_d3d_hle_resolve_loaded_xbe" not in entry_body

worker_start = SOURCE.index("static void xemu_d3d_hle_discovery_on_cpu")
worker_end = SOURCE.index("static bool xemu_d3d_hle_is_entry", worker_start)
worker_body = SOURCE[worker_start:worker_end]
assert "xemu_d3d_hle_resolve_loaded_xbe" in worker_body
assert "s_discovery_job_generation" in worker_body

print("d3d_hle_async_discovery_contract: OK")
