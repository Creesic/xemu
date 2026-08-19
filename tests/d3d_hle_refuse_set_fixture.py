import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEF = (ROOT / "thirdparty/xbsymbol-database/include/xref/d3d8.def").read_text()
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
PROFILES = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_profiles.c").read_text()
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

symbols = re.findall(r"^XREF_SYMBOL\(([^)]+)\)", DEF, re.MULTILINE)
functions = [
    name for name in symbols
    if not name.startswith(("D3D_g_", "D3DRS_", "D3DTSS_"))
    and not name.endswith("_OFFSET")
    and "GenericFragment" not in name
]
assert len(functions) > 100

for name in (
    "D3DDevice_Present",
    "D3DDevice_BeginPush",
    "D3DDevice_CreateTexture",
    "D3D_CDevice_SetStateVB",
    "D3DDevice_IsBusy",
):
    assert name in functions

assert "discovery_name_is_object_returning" in DISCOVERY
assert "discovery_name_is_native_safe" in DISCOVERY
assert "discovery_note_unsupported" in DISCOVERY
assert "unsupported_mutating_functions" in DISCOVERY
assert "return false;" in DISCOVERY  # default classifier is refuse-safe
assert "reviewed_blocker_count != 0u" in PROFILES
assert "xemu_d3d_hle_profile_validate" in HLE

print(f"d3d_hle_refuse_set_fixture: OK ({len(functions)} functions)")
