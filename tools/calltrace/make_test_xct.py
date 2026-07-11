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

# One 6-dword arg snapshot per event (parallel to EVENT_STREAM). Repeats
# create dedup; edge 3 and edge 0 each get a second distinct set.
ARG_STREAM = [
    (0x00011A40, 0, 5, 0, 0, 0),      # ev0  edge0 -> set0
    (1, 2, 3, 4, 5, 6),               # ev1  edge1 -> set0
    (0xDEAD, 0xBEEF, 0, 0, 0, 0),     # ev2  edge2 -> set0
    (7, 7, 7, 7, 7, 7),               # ev3  edge3 -> set0
    (7, 7, 7, 7, 7, 7),               # ev4  edge3 -> set0 (dup)
    (8, 8, 8, 8, 8, 8),               # ev5  edge3 -> set1 (new)
    (0x80012345, 0, 0, 0, 0, 0),      # ev6  edge4 -> set0
    (0x80012345, 0, 0, 0, 0, 0),      # ev7  edge4 -> set0 (dup)
    (0, 0, 0, 0, 0, 0),               # ev8  edge6 -> set0
    (0x16000, 0, 0, 0, 0, 0),         # ev9  edge7 -> set0
    (0x12000, 1, 2, 3, 4, 5),         # ev10 edge8 -> set0
    (0x00011A44, 0, 6, 0, 0, 0),      # ev11 edge0 -> set1 (new)
    (0xDEAD, 0xBEEF, 0, 0, 0, 0),     # ev12 edge2 -> set0 (dup)
]
ARGSET_DWORDS = 6
ARGSET_CAP = 16
ARGSET_CAP_EXTREME = 512   # >255 -> u16 nsets/index widths


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


def build_data(cap=ARGSET_CAP):
    import zlib
    wide = cap > 255                       # nsets/index become u16
    nfmt = '<H' if wide else '<B'
    ovf = 0xFFFF if wide else 0xFF
    base = bytearray(build())              # v1 body
    struct.pack_into('<I', base, 4, 3)     # version 1 -> 3

    # v2 event block (identical to build_timed()).
    ev = struct.pack('<%dI' % len(EVENT_STREAM), *EVENT_STREAM)
    ev_blob = zlib.compress(ev, 9)
    out = base
    out += struct.pack('<Q', len(EVENT_STREAM))
    out += struct.pack('<III', 0, 256, 64)
    out += struct.pack('<Q', len(ev))
    out += struct.pack('<Q', len(ev_blob))
    out += ev_blob

    # Intern arg snapshots per edge (same rule as ct_argset_intern).
    tables = [[] for _ in range(len(EDGES))]   # edge -> list of tuples
    index = []
    for edge_idx, snap in zip(EVENT_STREAM, ARG_STREAM):
        t = tables[edge_idx]
        if snap in t:
            index.append(t.index(snap))
        elif len(t) < cap:
            index.append(len(t))
            t.append(snap)
        else:
            index.append(ovf)

    # Arg-set table blob: per edge nsets (u8/u16), then nsets * 6 * u32.
    table = bytearray()
    for t in tables:
        table += struct.pack(nfmt, len(t))
        for snap in t:
            table += struct.pack('<6I', *snap)
    idx = b''.join(struct.pack(nfmt, v) for v in index)
    tcomp = zlib.compress(bytes(table), 9)
    icomp = zlib.compress(idx, 9)

    out += struct.pack('<II', ARGSET_DWORDS, cap)
    out += struct.pack('<Q', len(table))
    out += struct.pack('<Q', len(tcomp))
    out += struct.pack('<Q', len(idx))
    out += struct.pack('<Q', len(icomp))
    out += tcomp
    out += icomp
    return bytes(out)


if __name__ == '__main__':
    timed = '--timed' in sys.argv
    data = '--data' in sys.argv
    extreme = '--extreme' in sys.argv
    args = [a for a in sys.argv[1:]
            if a not in ('--timed', '--data', '--extreme')]
    default = ('test-fixture-data-extreme.xct' if extreme
               else 'test-fixture-data.xct' if data
               else 'test-fixture-timed.xct' if timed
               else 'test-fixture.xct')
    path = args[0] if args else default
    payload = (build_data(ARGSET_CAP_EXTREME) if extreme
               else build_data() if data
               else build_timed() if timed else build())
    open(path, 'wb').write(payload)
    print(f'wrote {path}')
