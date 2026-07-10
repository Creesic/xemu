#!/usr/bin/env python3
"""Generate a synthetic .xct fixture with a known call graph for viewer tests."""
import struct
import sys


def build():
    strtab = bytearray(b'\0')

    def add_str(name):
        if not name:
            return 0
        off = len(strtab)
        strtab.extend(name.encode() + b'\0')
        return off

    sections = [(add_str('.text'), 0x11000, 0x8000)]
    kimports = [(0x80012345, add_str('KeQuerySystemTime'))]
    edges = [
        (0x11010, 0x12000, 5),     # A -> B
        (0x11020, 0x13000, 1),     # A -> C
        (0x12040, 0x13000, 9),     # B -> C
        (0x12050, 0x14000, 2),     # B -> D1 (polymorphic site...)
        (0x12050, 0x15000, 4),     # B -> D2 (...same call site)
        (0x13008, 0x80012345, 3),  # C -> kernel leaf
        (0x13010, 0x13000, 2),     # C -> C recursion
        (0x80020000, 0x16000, 1),  # kernel -> T (thread root)
        (0x16008, 0x12000, 7),     # T -> B
    ]
    title = 'Test Fixture'.encode().ljust(88, b'\0')
    hdr = struct.pack('<10I', 0x52544358, 1, 0, 0x4D530001, 0x10000,
                      0x11000, len(sections), len(kimports), len(edges),
                      len(strtab))
    out = bytearray(hdr + title + strtab)
    for rec in sections:
        out += struct.pack('<3I', *rec)
    for rec in kimports:
        out += struct.pack('<2I', *rec)
    for site, callee, cnt in edges:
        out += struct.pack('<IIQ', site, callee, cnt)
    return bytes(out)


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'test-fixture.xct'
    open(path, 'wb').write(build())
    print(f'wrote {path}')
