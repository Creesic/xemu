#!/usr/bin/env python3
"""Parse, validate, and pretty-print an xemu .xct call-trace recording."""
import struct
import sys


def main(path):
    data = open(path, 'rb').read()
    (magic, ver, flags, title_id, base, entry,
     nsec, nkimp, nedge, stsz) = struct.unpack_from('<10I', data, 0)
    assert magic == 0x52544358, 'bad magic (not an .xct file)'
    assert ver == 1, f'unsupported version {ver}'
    title = data[40:128].split(b'\0')[0].decode('utf-8', 'replace')
    expect = 128 + stsz + nsec * 12 + nkimp * 8 + nedge * 16
    assert len(data) == expect, f'size mismatch: {len(data)} != {expect}'

    off = 128
    st = data[off:off + stsz]
    off += stsz

    def s(o):
        return st[o:st.index(b'\0', o)].decode('utf-8', 'replace')

    print(f'title={title!r} id={title_id:08X} base={base:08X} '
          f'entry={entry:08X} truncated={bool(flags & 1)}')
    print(f'{nsec} sections, {nkimp} kernel imports, {nedge} edges')
    for _ in range(nsec):
        no, start, size = struct.unpack_from('<3I', data, off)
        off += 12
        print(f'  section {s(no):10s} {start:08X}+{size:X}')
    for i in range(nkimp):
        addr, no = struct.unpack_from('<2I', data, off)
        off += 8
        if i < 5:
            print(f'  kimport {addr:08X} {s(no)}')
    if nkimp > 5:
        print(f'  ... {nkimp - 5} more kernel imports')
    total = 0
    for i in range(nedge):
        site, callee, cnt = struct.unpack_from('<IIQ', data, off)
        off += 16
        total += cnt
        if i < 10:
            print(f'  edge {site:08X} -> {callee:08X} x{cnt}')
    if nedge > 10:
        print(f'  ... {nedge - 10} more edges')
    print(f'total calls recorded: {total}')
    print('OK')


if __name__ == '__main__':
    main(sys.argv[1])
