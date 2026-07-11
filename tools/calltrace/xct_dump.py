#!/usr/bin/env python3
"""Parse, validate, and pretty-print an xemu .xct call-trace recording."""
import struct
import sys


def main(path):
    data = open(path, 'rb').read()
    (magic, ver, flags, title_id, base, entry,
     nsec, nkimp, nedge, stsz) = struct.unpack_from('<10I', data, 0)
    assert magic == 0x52544358, 'bad magic (not an .xct file)'
    assert ver in (1, 2, 3), f'unsupported version {ver}'
    title = data[40:128].split(b'\0')[0].decode('utf-8', 'replace')
    expect = 128 + stsz + nsec * 12 + nkimp * 8 + nedge * 16
    assert len(data) >= expect, f'size mismatch: {len(data)} < {expect}'

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
    if ver >= 2:
        import zlib
        ecount, = struct.unpack_from('<Q', data, off); off += 8
        eflags, tfull, tevery = struct.unpack_from('<III', data, off); off += 12
        raw_bytes, = struct.unpack_from('<Q', data, off); off += 8
        comp_bytes, = struct.unpack_from('<Q', data, off); off += 8
        ev = zlib.decompress(data[off:off + comp_bytes])
        assert len(ev) == raw_bytes, f'event size {len(ev)} != {raw_bytes}'
        idxs = struct.unpack('<%dI' % ecount, ev) if ecount else ()
        assert all(i < nedge for i in idxs), 'event index out of range'
        print(f'events: {ecount} (flags={eflags} throttle={tfull}/{tevery}, '
              f'{comp_bytes} compressed bytes)')
        print('first 20 event edge-indices:', list(idxs[:20]))
        off += comp_bytes
    if ver == 3:
        argset_dwords, argset_cap = struct.unpack_from('<II', data, off)
        off += 8
        table_raw, table_comp = struct.unpack_from('<QQ', data, off)
        off += 16
        index_raw, index_comp = struct.unpack_from('<QQ', data, off)
        off += 16
        table = zlib.decompress(data[off:off + table_comp])
        off += table_comp
        index = zlib.decompress(data[off:off + index_comp])
        off += index_comp
        assert len(table) == table_raw, f'table {len(table)} != {table_raw}'
        assert len(index) == index_raw, f'index {len(index)} != {index_raw}'
        wide = argset_cap > 255          # nsets/index are u16 above 255
        iw = 2 if wide else 1
        nidx = len(index) // iw
        assert nidx == ecount, 'index not parallel to events'
        o = 0
        total_sets = multi = 0
        for _ in range(nedge):
            if wide:
                nsets = int.from_bytes(table[o:o + 2], 'little')
                o += 2
            else:
                nsets = table[o]
                o += 1
            o += nsets * argset_dwords * 4
            total_sets += nsets
            if nsets > 1:
                multi += 1
        if wide:
            overflow = sum(1 for k in range(0, len(index), 2)
                           if index[k] == 0xFF and index[k + 1] == 0xFF)
        else:
            overflow = index.count(0xFF)
        print(f'data: {argset_dwords} dwords/set, cap {argset_cap}, '
              f'{total_sets} sets across {nedge} edges ({multi} multi-set), '
              f'{nidx} indices, {overflow} overflow')
    print('OK')


if __name__ == '__main__':
    main(sys.argv[1])
