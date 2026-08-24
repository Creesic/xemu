import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
cc = os.environ.get("CC", "cc")

with tempfile.TemporaryDirectory(prefix="d3d-hle-shader-constant-") as tmp:
    exe = Path(tmp) / "d3d_hle_shader_constant_semantic_test.exe"
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "hw/xbox/d3d_hle"),
            str(ROOT / "tests/d3d_hle_shader_constant_semantic_test.c"),
            "-o",
            str(exe),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(exe)], check=True, cwd=ROOT)

DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
API = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest_api.c").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
assert "B1(D3DDevice_SetShaderConstantMode" in DISCOVERY
assert "d3d_hle_device_set_shader_constant_mode_std" in API
assert "d3d_hle_guest_set_shader_constant_mode" in GUEST
assert "d3d_hle_guest_set_vertex_shader_constant_hardware" in GUEST
fast_start = API.index("void d3d_hle_device_set_vertex_shader_constant1_fast")
fast_end = API.index("\n}", fast_start)
assert "d3d_hle_guest_set_vertex_shader_constant_hardware" in API[fast_start:fast_end]


def extract_function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : i + 1]
    raise AssertionError(f"unbalanced braces in {name}")


# Contract: guest-controlled vertex-shader constant arguments must never
# reach d3d_hle_guest_fatal. Proven by crash dump
# qemu-system-i386w.exe.109832.dmp (FAST_FAIL_FATAL_APP_EXIT, PGR2 retail
# profile active): the working-tree ±96 constant-mode translator aborted on
# start=96 because the XDK helper bodies hand these hooks HARDWARE constant
# slots already (public domain tops out at 95); translating them again
# corrupted every transform constant (uniform green screen) and rejected
# legitimate hardware indices. The committed MM3-validated implementation
# passes registers through raw.
PUBLIC_BODY = extract_function(
    GUEST,
    "void d3d_hle_guest_set_vertex_shader_constant(\n    int32_t",
)
assert "d3d_hle_guest_fatal" not in PUBLIC_BODY
assert "xbox_d3d8_shader_constant_index" not in PUBLIC_BODY

HARDWARE_BODY = extract_function(
    GUEST,
    "void d3d_hle_guest_set_vertex_shader_constant_hardware(\n    uint32_t",
)
# Host-lifecycle invariants stay fatal; guest-controlled range violations
# must be tolerated (one-shot diagnostic + drop).
assert '"SetVertexShaderConstant hardware range"' not in HARDWARE_BODY
assert '"SetVertexShaderConstant",' in HARDWARE_BODY

MODE_BODY = extract_function(GUEST, "void d3d_hle_guest_set_shader_constant_mode(")
assert "d3d_hle_guest_fatal" not in MODE_BODY
print("d3d_hle_shader_constant_mode_contract: OK")
