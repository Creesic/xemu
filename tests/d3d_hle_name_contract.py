import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
cc = os.environ.get("CC", "cc")

with tempfile.TemporaryDirectory(prefix="d3d-hle-name-") as tmp:
    exe = Path(tmp) / "d3d_hle_name_semantic_test.exe"
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "hw/xbox/d3d_hle"),
            str(ROOT / "tests/d3d_hle_name_semantic_test.c"),
            "-o",
            str(exe),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(exe)], check=True, cwd=ROOT)

DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
assert "xemu_d3d_hle_canonical_name_length" in DISCOVERY
assert "static size_t canonical_name_length" not in DISCOVERY
print("d3d_hle_name_contract: OK")
