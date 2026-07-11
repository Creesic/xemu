# `.xct` Call-Trace File Format

An `.xct` file is a compact binary recording of a running Xbox game's function
call graph, produced by xemu's **Debug → Call Trace** feature and read by
`viewer.html`, `xct_dump.py`, and `make_test_xct.py`.

There are two versions:

- **v1 (Edges)** — deduplicated `(call_site → callee)` edges with hit counts.
- **v2 (Timed)** — everything in v1, plus a compressed, ordered log of every
  recorded call (an event stream) for the timeline/playback view.
- **v3 (Data)** — everything in v2, plus per-call argument snapshots: a
  per-edge table of distinct argument-sets and a 1-byte-per-event index into it.

Everything is **little-endian**. All offsets and sizes below are in bytes.

---

## Overall layout

```
+------------------------------------------------------+
| Header                        128 bytes              |
+------------------------------------------------------+
| String table                  strtab_size bytes      |
+------------------------------------------------------+
| Sections           section_count × 12 bytes          |
+------------------------------------------------------+
| Kernel imports     kimport_count × 8 bytes           |
+------------------------------------------------------+
| Edges              edge_count × 16 bytes              |
+------------------------------------------------------+
| Event block        (v2+) 36-byte sub-header +        |
|                    deflate-compressed u32 stream      |
+------------------------------------------------------+
| Data block         (v3 only) 40-byte sub-header +    |
|                    two deflate streams (table, index) |
+------------------------------------------------------+
```

A v1 file ends after the Edges block. A v2 file appends the Event block. A v3
file also appends the Data block (and always contains the Event block).

---

## Header (128 bytes)

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0x00 | 4 | u32 | `magic` | `0x52544358` — the ASCII bytes `X C T R` |
| 0x04 | 4 | u32 | `version` | `1` = Edges, `2` = Timed, `3` = Data |
| 0x08 | 4 | u32 | `flags` | bit 0 = the edge map hit its 1M-edge cap (graph is partial) |
| 0x0C | 4 | u32 | `title_id` | XBE certificate title id |
| 0x10 | 4 | u32 | `base` | XBE base address (usually `0x00010000`) |
| 0x14 | 4 | u32 | `entry` | XBE entry point, already de-obfuscated |
| 0x18 | 4 | u32 | `section_count` | number of XBE section records |
| 0x1C | 4 | u32 | `kimport_count` | number of kernel-import records |
| 0x20 | 4 | u32 | `edge_count` | number of edge records |
| 0x24 | 4 | u32 | `strtab_size` | size of the string table, in bytes |
| 0x28 | 88 | char[88] | `title_name` | title, UTF-8, NUL-padded (not necessarily NUL-terminated if 88 bytes) |

The header is always exactly 128 bytes; the string table starts at offset 128.

## String table (`strtab_size` bytes)

A blob of NUL-separated UTF-8 strings. Section names and kernel-import names
are given as **byte offsets into this table**. Offset `0` is always the empty
string `""` (the first byte is a `\0`).

To read a name at offset `o`: read bytes from `strtab[o]` up to (not including)
the next `\0`.

## Sections (`section_count` × 12 bytes)

The XBE's section table — used to color/label functions by which library they
belong to (`.text`, `D3D`, `XGRPH`, …).

| Offset | Size | Type | Field |
|---|---|---|---|
| +0 | 4 | u32 | `name_off` (offset into string table) |
| +4 | 4 | u32 | `virtual_start` |
| +8 | 4 | u32 | `virtual_size` |

A function at address `a` belongs to the section whose
`[virtual_start, virtual_start + virtual_size)` contains `a`.

## Kernel imports (`kimport_count` × 8 bytes)

Maps resolved Xbox kernel export addresses to names, so calls into the kernel
(callee ≥ `0x80000000`) can be labeled (`NtClose`, `KeQuerySystemTime`, …).

| Offset | Size | Type | Field |
|---|---|---|---|
| +0 | 4 | u32 | `addr` (kernel export virtual address) |
| +4 | 4 | u32 | `name_off` (offset into string table) |

## Edges (`edge_count` × 16 bytes)

The deduplicated call graph. One record per unique `(call_site, callee)` pair.

| Offset | Size | Type | Field |
|---|---|---|---|
| +0 | 4 | u32 | `call_site` — address of the `CALL` instruction |
| +4 | 4 | u32 | `callee` — call target address |
| +8 | 8 | u64 | `count` — number of times this exact call fired |

Notes:

- `call_site` and `callee` are guest virtual addresses. A `callee ≥ 0x80000000`
  is a kernel call (look it up in the kernel-import table).
- Edges where **both** endpoints are ≥ `0x80000000` (kernel-internal) are not
  recorded.
- In **v2**, edges are written in **index order**: `edges[k]` is the edge whose
  event-stream index is `k` (see below). In v1 the order is unspecified.

---

## Event block (v2 only)

Appended immediately after the Edges block. Present iff `version == 2`.

### Sub-header (36 bytes)

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| +0 | 8 | u64 | `event_count` | number of events (calls) in the log |
| +8 | 4 | u32 | `event_flags` | bit 0 = the event log hit its cap (timeline is partial) |
| +12 | 4 | u32 | `throttle_full` | log every call up to this many per edge (e.g. 256) |
| +16 | 4 | u32 | `throttle_every` | then log 1-in-this-many (e.g. 64) |
| +20 | 8 | u64 | `raw_bytes` | uncompressed stream size = `event_count × 4` |
| +28 | 8 | u64 | `comp_bytes` | size of the compressed blob that follows |

### Compressed event stream (`comp_bytes` bytes)

A **zlib-deflated** (RFC 1950, i.e. a normal `zlib.compress` / `DecompressionStream('deflate')` stream) array of `event_count` little-endian `u32` values.

Each value is an **index into the Edges array**. Decompress to get
`event_count × 4` bytes, then read as `u32[event_count]`.

**Semantics:** the log is every recorded call, in execution order. An event's
*position in the stream is its timestamp* (call-sequence number) — event 0
happened first. To find which call an event is, look up `edges[value]`.

**Throttling:** to keep the log small, each edge logs its first `throttle_full`
calls, then only every `throttle_every`-th call after that. So the number of
events for an edge is ≤ its `count`; the `count` in the Edges block is always
the exact total. Rank functions by inbound `count` (not by event frequency) to
find the noisiest ones.

---

## Data block (v3 only)

Appended immediately after the Event block. Present iff `version == 3`. A v3
file is a full v2 file (it always contains the Event block) plus this block.

### Sub-header (40 bytes)

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| +0 | 4 | u32 | `argset_dwords` | dwords per arg-set (always 6: ECX, EDX, [ESP+0..12]) |
| +4 | 4 | u32 | `argset_cap` | max distinct sets per edge (64 in current builds; always read this field) |
| +8 | 8 | u64 | `table_raw_bytes` | uncompressed arg-set table size |
| +16 | 8 | u64 | `table_comp_bytes` | compressed arg-set table size |
| +24 | 8 | u64 | `index_raw_bytes` | uncompressed index size = `event_count` |
| +32 | 8 | u64 | `index_comp_bytes` | compressed index size |

### Arg-set table (`table_comp_bytes`, zlib)

Decompresses to `table_raw_bytes`. A sequential walk over **all edges, in
index order** (`edge_count` entries):

```
for each edge:
    u8  nsets                         # 0..16
    nsets × (argset_dwords × u32)     # ECX, EDX, [ESP+0], [ESP+4], [ESP+8], [ESP+12]
```

Edge `k`'s distinct arg-sets are the `k`-th entry.

### Index stream (`index_comp_bytes`, zlib)

Decompresses to `event_count` bytes, one `u8` per event, parallel to the event
stream. For event `i`, the arg-set is `edges[events[i]].argsets[index[i]]`,
unless `index[i] == 0xFF` (**overflow** — that call's snapshot was a distinct
set beyond `argset_cap` and was not stored).

**Semantics:** the snapshot is the outgoing argument words *at the call*, read
before the return address is pushed — so the stack dwords are the caller's
pushed cdecl/stdcall arguments and ECX/EDX are the thiscall/fastcall register
arguments. Only the words are stored, not the memory they point at.

---

## Deriving functions (optional)

The file records call *sites* and *targets*, not function boundaries. The
viewer infers functions with the **nearest-preceding-entry** rule:

1. The set of function entry points = every distinct `callee` (in game space,
   `< 0x80000000`) plus the XBE `entry`.
2. A `call_site` belongs to the function with the greatest entry address ≤ the
   call site.
3. Merge edges from call-site granularity to function→function granularity.

This is a heuristic: a function that is never called (so never appears as a
callee) is not identified as its own entry, and its calls are attributed to the
preceding known function. Load a symbol map (address → name) for real names.

---

## Minimal parser (Python)

```python
import struct, zlib

def read_xct(path):
    d = open(path, 'rb').read()
    (magic, ver, flags, title_id, base, entry,
     nsec, nkimp, nedge, stsz) = struct.unpack_from('<10I', d, 0)
    assert magic == 0x52544358 and ver in (1, 2, 3)
    title = d[40:128].split(b'\0')[0].decode('utf-8', 'replace')

    off = 128
    strtab = d[off:off + stsz]; off += stsz
    name = lambda o: strtab[o:strtab.index(b'\0', o)].decode('utf-8', 'replace')

    sections = []
    for _ in range(nsec):
        no, start, size = struct.unpack_from('<3I', d, off); off += 12
        sections.append((name(no), start, size))

    kimports = {}
    for _ in range(nkimp):
        addr, no = struct.unpack_from('<2I', d, off); off += 8
        kimports[addr] = name(no)

    edges = []
    for _ in range(nedge):
        site, callee, cnt = struct.unpack_from('<IIQ', d, off); off += 16
        edges.append((site, callee, cnt))

    events = None
    if ver >= 2:
        ecount, = struct.unpack_from('<Q', d, off); off += 8
        eflags, tfull, tevery = struct.unpack_from('<III', d, off); off += 12
        raw_bytes, = struct.unpack_from('<Q', d, off); off += 8
        comp_bytes, = struct.unpack_from('<Q', d, off); off += 8
        raw = zlib.decompress(d[off:off + comp_bytes])
        assert len(raw) == raw_bytes
        events = struct.unpack('<%dI' % ecount, raw)   # indices into edges
        off += comp_bytes

    argsets = None       # per edge: list of arg-set tuples
    argidx = None        # per event: index into that edge's arg-sets (0xFF = overflow)
    if ver >= 3:
        adw, acap = struct.unpack_from('<2I', d, off); off += 8
        traw, tcomp = struct.unpack_from('<2Q', d, off); off += 16
        iraw, icomp = struct.unpack_from('<2Q', d, off); off += 16
        table = zlib.decompress(d[off:off + tcomp]); off += tcomp
        argidx = list(zlib.decompress(d[off:off + icomp])); off += icomp
        o = 0
        argsets = []
        for _ in range(nedge):
            nsets = table[o]; o += 1
            sets = []
            for _ in range(nsets):
                sets.append(struct.unpack_from('<%dI' % adw, table, o))
                o += adw * 4
            argsets.append(sets)

    return dict(version=ver, title=title, title_id=title_id, base=base,
                entry=entry, sections=sections, kimports=kimports,
                edges=edges, events=events, argsets=argsets, argidx=argidx)
```

## Minimal event inflation (JavaScript / browser)

```js
async function inflateEvents(compBytes /* Uint8Array */) {
  const ds = new DecompressionStream('deflate');
  const raw = await new Response(
      new Blob([compBytes]).stream().pipeThrough(ds)).arrayBuffer();
  return new Uint32Array(raw);        // indices into the edges array
}
```

---

## Reference implementations

- `xct_dump.py` — parse + validate + pretty-print any `.xct` (v1 or v2).
- `make_test_xct.py` — build synthetic `.xct` fixtures (`--timed` for v2).
- `viewer.html` — full parser in `parseXCT()` / `inflateEvents()`.
- The writer lives in xemu at `xemu-calltrace.c` (`xemu_calltrace_save`).
