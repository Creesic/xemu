"""Contract: registered even XDK shader pointers resolve without hiding FVFs."""

from pathlib import Path


GUEST = (
    Path(__file__).resolve().parents[1]
    / "hw/xbox/d3d_hle/d3d_hle_guest.c"
).read_text()


def function(signature):
    start = GUEST.index(signature)
    brace = GUEST.index("{", start)
    depth = 0
    for index in range(brace, len(GUEST)):
        if GUEST[index] == "{":
            depth += 1
        elif GUEST[index] == "}":
            depth -= 1
            if depth == 0:
                return GUEST[start : index + 1]
    raise AssertionError(f"unbalanced function: {signature}")


lookup = function("d3d_hle_guest_find_vertex_shader_resource(")
assert "shader_handle & ~1u" in lookup
assert "!(shader_handle & 1u)" in lookup
assert "object_va - 0x14u" in lookup

set_shader = function("HRESULT d3d_hle_guest_set_vertex_shader(")
assert "d3d_hle_guest_find_vertex_shader_resource(shader_handle)" in set_shader
assert "host_handle = shader->host_handle" in set_shader
assert "else if (shader_handle & 1u)" in set_shader

print("d3d_hle_even_vertex_shader_handle_contract: OK")
