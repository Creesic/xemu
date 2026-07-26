# Frame Inspector Capture File Formats (`XEMUFC` / `XEMUXCT`)

Binary recordings of a single captured frame produced by xemu's **Frame
Inspector** save/export. Two encodings exist:

- **`XEMUFC`** — deflate-compressed. Written with a `.xfc` extension.
- **`XEMUXCT`** — uncompressed. Written with a `.xct` extension.

They carry the same chunk set; only the container framing and compression
differ.

> **Not to be confused with `XCT_FORMAT.md` in this directory.** That documents
> the **call-trace** format, whose magic is `XCTR` (`0x52544358`). Despite both
> using a `.xct` extension, they are unrelated formats with different magics,
> different headers, and different producers. Check the magic, not the
> extension — captures in the wild are also sometimes saved with a `.txt`
> extension, which is likewise not a reliable signal.

Everything is **little-endian**. Sizes and offsets are in bytes.

---

## Overall layout

```
+---------------------------------------------+
| File header      header_size bytes          |
+---------------------------------------------+
| Chunk 0 header   chunk_header_size bytes    |
| Chunk 0 payload                             |
+---------------------------------------------+
| Chunk 1 header   ...                        |
| Chunk 1 payload                             |
+---------------------------------------------+
| ... until EOF                               |
+---------------------------------------------+
```

There is no chunk count in the header and no index; walk the file by reading a
chunk header, consuming its payload, and repeating until EOF.

---

## File header

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0x00 | 8 | char[8] | `magic` | `"XEMUFC\0\0"` or `"XEMUXCT\0"` |
| 0x08 | 4 | u32 | `version` | `1` observed |
| 0x0C | 4 | u32 | `endian_marker` | `0x01020304` when the reader's endianness matches |
| 0x10 | 4 | u32 | `header_size` | total header size; payload/chunks start here |
| 0x14 | 4 | u32 | `chunk_header_size` | size of **every** chunk header |
| 0x18 | 4 | u32 | `pointer_size` | `8` on the writing host |
| 0x1C | 4 | u32 | `bool_size` | `1` on the writing host |
| 0x20 | 4 | u32 | `flags` | capture-level flags |
| 0x24 | … | u64 × 3 | `capture_id`, `budget_limit`, `budget_used` | field offsets vary with padding — see below |

**Always seek to `header_size` before reading the first chunk, and always use
`chunk_header_size` from the file when parsing chunk headers.** These two
fields make the container self-describing, which matters because the structs
have changed between builds (see *Version drift*).

Observed values:

| | `XEMUFC` | `XEMUXCT` |
|---|---|---|
| `header_size` | 64 | 56 |
| `chunk_header_size` | 64 | 56 |

---

## Chunk header

Common prefix, identical in both formats:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0x00 | 24 | char[24] | `name` — NUL-padded ASCII, e.g. `methods` |
| 0x18 | 4 | u32 | `element_size` — bytes per record, or `1` for blobs/text |
| 0x1C | 4 | u32 | `index` — `0xFFFFFFFF` when unused |
| 0x20 | 4 | u32 | `subindex` — `0xFFFFFFFF` when unused |
| 0x24 | 4 | u32 | `flags` — see below |
| 0x28 | 8 | u64 | `count` — number of elements |
| 0x30 | 8 | u64 | `byte_size` — uncompressed payload size |

`XEMUFC` then adds one more field; `XEMUXCT` (as written by
`0.8.136-130-g2f08367b6f`) does **not**:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0x38 | 8 | u64 | `stored_size` — bytes actually on disk (`XEMUFC` only) |

**Payload length on disk** is `stored_size` when the field is present, and
`byte_size` otherwise. A parser that keys off `chunk_header_size` handles both:
`56` ⇒ no `stored_size`, `64` ⇒ present.

### Chunk flags

| Bit | Value | Meaning |
|---|---|---|
| 0 | `0x1` | payload is an array of native C structs |
| 1 | `0x2` | payload is NUL-terminated UTF-8 text |
| 2 | `0x4` | payload is deflate-compressed (inflate to `byte_size` bytes) |

`XEMUFC` chunks are typically `0x5` (native struct + deflate); `XEMUXCT`
chunks are typically `0x1` (native struct, raw).

---

## Chunks

Emitted in this order. Names are stable; presence depends on what the capture
recorded.

| Name | Contents |
|---|---|
| `build.version`, `build.commit`, `build.date` | text, identifies the writing build |
| `capture.meta` | one `FIXctCaptureMeta` — counts, caps, truncation flags |
| `surfaces` | colour/zeta surface records |
| `resources` | `FIResource` table (see below) |
| `resource.blob` | packed bytes all `FIResource.off` values index into |
| `resource.hash` | hash → resource-id lookup slots |
| `events` | frame event list (batches, clears, flips) |
| `methods` | `FIMethodRec` — every recorded NV2A method write |
| `method.batches` | `FIMethodBatch` — maps a batch event to its method range |
| `commands` | pushbuffer command records |
| `draw.submissions` | per-submission state (large struct, 1360 B observed) |
| `draw.segments`, `draw.indices`, `draw.samples`, `draw.sources` | geometry and provenance |
| `draw.writer_sets`, `draw.writers` | which guest code wrote each resource |
| `setter.sources`, `setter.destinations` | state-setter provenance |
| `batch.resources` | batch → resource id links |
| `origin.nodes`, `origin.argsets` | call-origin attribution graph |
| `hist.*` | surface history: `hist.keyframe.image` and `hist.current` are raw RGBA framebuffers |

### Record layouts

Authoritative definitions live in `xemu-frameinspect-*.h`; these are the ones
most useful when reading a capture externally.

```c
typedef struct FIMethodRec {      /* 20 bytes -- "methods" */
    uint32_t method;              /* NV097_* method offset, or FI_METHOD_RAW_WORD */
    uint16_t subchannel;
    uint16_t confidence;
    uint32_t param;
    uint32_t phys_addr;
    uint32_t writer_node;
} FIMethodRec;

typedef struct FIMethodBatch {    /* 12 bytes -- "method.batches" */
    uint32_t batch_event;
    uint32_t first_rec;           /* index into "methods" */
    uint32_t rec_count;
} FIMethodBatch;

typedef struct FIResource {       /* 32 bytes -- "resources" */
    uint32_t kind;
    uint32_t len;
    uint64_t off;                 /* offset into "resource.blob" */
    uint64_t meta;
    uint64_t hash;
} FIResource;
```

### `hist.keyframe.image` / `hist.current`

Raw RGBA framebuffers, `count` pixels of 4 bytes. A 640×480 guest surface
captured at xemu's 2× scale yields `count == 1228800` (1280×960).

A capture contains **many** of these — one series per surface, sampled at
several points in time, including small surfaces (32×32, 64×64, …). Selecting
"the last one" is meaningless; filter by `count` for the resolution you want
and correlate with the `surfaces` chunk to identify which surface it is.

---

## Reading per-batch state

**Do not reconstruct a batch's pipeline state by replaying the `methods` log in
submission order.** It does not reproduce what the batch actually used —
replaying for one MM3 batch yielded `color_mask=ARGB, blend=0` where that
batch's own Frame Inspector export reported `color_mask=0xe, blend=1`.

Each batch instead references register/program/constant snapshot resources —
the `resources regs=… program=… constants=…` line in a submission export.
Resolve those ids through the `resources` table into `resource.blob`; that is
the authoritative state.

Method replay *is* reliable for global, unbatched operations such as clears
(`NV097_SET_COLOR_CLEAR_VALUE`, `NV097_SET_ZSTENCIL_CLEAR_VALUE`,
`NV097_CLEAR_SURFACE`), where ordering in the log is unambiguous.

---

## Version drift

The `FIXctChunkHeader` struct in `ui/xui/frame-inspector.cc` currently declares
three trailing `u64` fields (`count`, `byte_size`, `stored_size`), giving a
64-byte header. Captures written by `0.8.136-130-g2f08367b6f` in the `XEMUXCT`
encoding declare `chunk_header_size = 56` and carry only two.

Parse defensively: take `header_size` and `chunk_header_size` from the file and
derive the layout from them rather than from the current struct definition.

---

## Minimal reader

```python
import struct

def chunks(path):
    f = open(path, "rb")
    hdr = f.read(64)
    magic = hdr[:8].rstrip(b"\0").decode()
    assert magic in ("XEMUFC", "XEMUXCT"), magic
    version, endian, header_size, chunk_hdr = struct.unpack_from("<4I", hdr, 8)
    assert endian == 0x01020304, "byte-swapped capture"
    f.seek(header_size)
    while True:
        ch = f.read(chunk_hdr)
        if len(ch) < chunk_hdr:
            return
        name = ch[:24].rstrip(b"\0").decode(errors="replace")
        if not name:
            return
        elem, index, subindex, flags = struct.unpack_from("<4I", ch, 24)
        count, byte_size = struct.unpack_from("<2Q", ch, 40)
        stored = struct.unpack_from("<Q", ch, 56)[0] if chunk_hdr >= 64 \
                 else byte_size
        payload = f.read(stored)
        if flags & 0x4:
            import zlib
            payload = zlib.decompress(payload)
        yield name, elem, count, flags, payload
```
