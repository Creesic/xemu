import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
API = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest_api.c").read_text()
OOVPA = "\n".join(
    (ROOT / path).read_text()
    for path in (
        "thirdparty/xbsymbol-database/src/OOVPADatabase/D3D8_OOVPA.c",
        "thirdparty/xbsymbol-database/src/OOVPADatabase/D3D8LTCG_OOVPA.c",
    )
)

start = DISCOVERY.index("static const XemuD3DHleBinding bindings[]")
end = DISCOVERY.index("\n};", start)
binding_body = DISCOVERY[start:end]
bindings = [
    (name, int(arity), entry)
    for _, arity, name, entry in re.findall(
        r"\b(MA|M|A|B)(\d)\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
        r"([A-Za-z_][A-Za-z0-9_]*)",
        binding_body,
        re.DOTALL,
    )
]
bindings.extend(
    (name, int(arity), "NULL")
    for arity, name in re.findall(
        r"\bN(\d)\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", binding_body
    )
)
bindings.extend(
    (name, int(arity), "NULL")
    for arity, name in re.findall(
        r"\bO(\d)\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", binding_body
    )
)

registered_names = set(
    re.findall(
        r"SYM_FUN(?:_LTCG)?\(\s*([A-Za-z_][A-Za-z0-9_]*)",
        OOVPA,
    )
)

missing_detector_names = sorted(
    {name for name, _, _ in bindings} - registered_names
)
assert not missing_detector_names, (
    "automatic bindings use names XbSymbolDatabase never emits: "
    + ", ".join(missing_detector_names)
)

binding_counts = Counter((name, arity) for name, arity, _ in bindings)
duplicates = sorted(key for key, count in binding_counts.items() if count != 1)
assert not duplicates, f"duplicate automatic binding rows: {duplicates}"

wrapper_arities = {
    entry: int(arity)
    for entry, arity in re.findall(
        r"DEFINE_STD_WRAPPER\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)",
        API,
    )
}
wrong_wrapper_arities = sorted(
    (name, arity, entry, wrapper_arities[entry])
    for name, arity, entry in bindings
    if entry in wrapper_arities and wrapper_arities[entry] != arity
)
assert not wrong_wrapper_arities, (
    "binding logical arity disagrees with its std wrapper: "
    + repr(wrong_wrapper_arities)
)

required = {
    ("Get2DSurfaceDesc", 3, "d3d_hle_get_2d_surface_desc"),
    ("D3DVertexBuffer_Lock2", 2, "d3d_hle_vertex_buffer_lock2_std"),
    (
        "D3DDevice_SetRenderTargetFast",
        3,
        "d3d_hle_device_set_render_target_fast_std",
    ),
    ("D3DDevice_CreatePalette2", 1, "d3d_hle_device_create_palette2_std"),
    (
        "D3DDevice_CreateImageSurface",
        4,
        "d3d_hle_device_create_image_surface_std",
    ),
    ("D3D_AllocContiguousMemory", 2, "NULL"),
    ("D3DDevice_BlockUntilVerticalBlank", 0, "NULL"),
}
missing_required = sorted(required - set(bindings))
assert not missing_required, f"reviewed shared bindings missing: {missing_required}"

assert '"CMiniport_Get"' in DISCOVERY
assert '"CMiniport_Is"' in DISCOVERY
assert '"D3D_CMiniport_Get"' not in DISCOVERY
assert '"D3D_CMiniport_Is"' not in DISCOVERY

print(
    "d3d_hle_detector_catalog_contract: OK "
    f"({len(bindings)} bindings, {len(registered_names)} callback names)"
)
