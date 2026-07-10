#!/usr/bin/env python3
"""Generate a synthetic .xct fixture with a known call graph for viewer tests."""
import struct
import sys

# Shared edge list (index order matters for the timed event stream).
EDGES = [
    (0x11010, 0x12000, 5),     # 0: A -> B
    (0x11020, 0x13000, 1),     # 1: A -> C
    (0x12040, 0x13000, 9),     # 2: B -> C
    (0x12050, 0x14000, 2),     # 3: B -> D1 (polymorphic site...)
    (0x12050, 0x15000, 4),     # 4: B -> D2 (...same call site)
    (0x13008, 0x80012345, 3),  # 5: C -> kernel leaf
    (0x13010, 0x13000, 2),     # 6: C -> C recursion
    (0x80020000, 0x16000, 1),  # 7: kernel -> T (thread root)
    (0x16008, 0x12000, 7),     # 8: T -> B
]
# A known event stream referencing EDGES by index.
EVENT_STREAM = [0, 1, 2, 3, 3, 3, 4, 4, 6, 7, 8, 0, 2]


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
    edges = EDGES
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


def build_timed():
    import zlib
    base = bytearray(build())              # v1 body
    struct.pack_into('<I', base, 4, 2)     # bump version 1 -> 2
    ev = struct.pack('<%dI' % len(EVENT_STREAM), *EVENT_STREAM)
    blob = zlib.compress(ev, 9)
    out = base
    out += struct.pack('<Q', len(EVENT_STREAM))     # event_count
    out += struct.pack('<III', 0, 256, 64)          # flags, throttle full/every
    out += struct.pack('<Q', len(ev))               # raw_bytes
    out += struct.pack('<Q', len(blob))             # comp_bytes
    out += blob
    return bytes(out)


if __name__ == '__main__':
    timed = '--timed' in sys.argv
    args = [a for a in sys.argv[1:] if a != '--timed']
    path = args[0] if args else ('test-fixture-timed.xct' if timed
                                 else 'test-fixture.xct')
    data = build_timed() if timed else build()
    open(path, 'wb').write(data)
    print(f'wrote {path}')
