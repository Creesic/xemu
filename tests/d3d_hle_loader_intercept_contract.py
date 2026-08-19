from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPU_H = (ROOT / "include/hw/core/cpu.h").read_text()
EXEC_C = (ROOT / "accel/tcg/cpu-exec.c").read_text()
TRANS_C = (ROOT / "accel/tcg/translator.c").read_text()

assert "exec_loader_pc[2]" in CPU_H
assert "exec_loader_return_pc" in CPU_H

assert "exec_loader_pc[0]" in EXEC_C
assert "exec_loader_pc[1]" in EXEC_C
assert "exec_loader_return_pc" in EXEC_C
assert "exec_loader_pc[0]" in TRANS_C
assert "exec_loader_pc[1]" in TRANS_C

print("d3d_hle_loader_intercept_contract: OK")
