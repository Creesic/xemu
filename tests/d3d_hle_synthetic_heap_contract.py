import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
assert CC, "no C compiler available for synthetic heap contract"

with tempfile.TemporaryDirectory(prefix="xemu-d3d-hle-heap-") as tmp:
    exe = Path(tmp) / "d3d_hle_synthetic_heap_test.exe"
    command = [
        CC,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(ROOT / "hw/xbox/d3d_hle"),
        str(ROOT / "tests/d3d_hle_synthetic_heap_test.c"),
        str(ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_guest_heap.c"),
        "-o",
        str(exe),
    ]
    subprocess.run(command, check=True, cwd=ROOT)
    subprocess.run([str(exe)], check=True, cwd=ROOT)

print("d3d_hle_synthetic_heap_contract: OK")
