# BeginStateBig recon — Commit B, step 1

Status: recon only. No `bindings[]` row added; the parameter is now named,
but the go/no-go below says this symbol does not belong in the automatic
REPLACE table on its own.

## Method

Census VAs were resolved to file offsets through each XBE section table and
disassembled with `objdump -b binary -m i386 -M intel`. The Forza census VA
`003BE090` is **not** the function: it is inside `Forza.xbe` but decodes as
garbage there. An opcode signature scan (wildcarding the two absolute
globals) found exactly one match per retail image and every other match
lined up with its census VA, so the scan is trustworthy:

| Title | Census VA | Disassembled VA | Image |
|---|---|---|---|
| PGR2 `0x4D53004B` | `001C6150` | `001C6150` | `default.xbe` |
| Sega GT Online `0x53450021` | `0020A120` | `0020A120` | `default.xbe` |
| Spider-Man 2 `0x4156002B` | `003C4970` | `003C4970` | `default.xbe` |
| Forza `0x4D53006E` | `003BE090` | `003BDD90` | `Extracted/Forza.xbe` |

The Forza row is **a build difference, not a spy defect**. Census `va=` is
`hook->address` from XbSDB, that PC took 10,148,881 hits, and the OOVPA
matches at offset 0 (`8B 0D` = `mov ecx,[pDevice]`), so the runtime is
naming a live entry. Re-running the signature scan across every Forza image
with title id `4D53006E` confirms it:

```
Extracted/Forza.xbe          MATCH at 003BDD90
uAll/default.xbe             MATCH at 003BE090
uAll/forza.xbe               MATCH at 003BE090
uXBLA/forza.xbe              MATCH at 003BE090
u/default.xbe                MATCH at 003BE090
UpdaterTest-.../Forza.xbe    MATCH at 003BDD90
```

The updated builds place the function at `003BE090`; the un-updated
`Extracted/Forza.xbe` places it at `003BDD90`. The 0x300 delta is the shift
between those two builds, not a virtual-vs-raw slip — the Forza.xbe section
table resolves `003BE090` cleanly (D3D section, delta `0x67B0`, inside
`rsize=0x10628`). **Trust the runtime hook address.** Any future
disassembly of Forza must use the same image the census ran against.

## The body (identical in all four titles)

```asm
mov  ecx, ds:pDevice      ; global CDevice*
mov  eax, [ecx]           ; pDevice->pPut       (current push pointer)
mov  ecx, [ecx+4]         ; pDevice->pPushLimit
push esi
mov  esi, [esp+8]         ; ARG: Count  (stdcall, 1 dword)
lea  edx, [eax+esi*4]     ; needed_end = pPut + Count*4
add  ecx, 0x200           ; limit + 512 slack
cmp  edx, ecx
jb   done                 ; enough room -> return immediately
mov  eax, ds:g_PushSize   ; grow path
mov  edx, eax
shr  eax, 1               ; half
lea  ecx, [esi*4+0x204]   ; Count*4 + 516
cmp  ecx, eax
jbe  .1
mov  eax, ecx             ; max(half, needed)
.1: cmp ecx, edx
jbe  .2
mov  edx, ecx
.2: push edx
push eax
call MakeRequestedSpace   ; (requested, maximum) -> stdcall ret 8
done:
pop  esi
ret  4                    ; <-- stdcall, ONE dword argument
```

Byte-for-byte identical across PGR2 / Sega GT / SM2 / Forza; only the two
absolute global addresses differ. Same `call` displacement `e8 70 fe ff ff`
in every image, i.e. the callee sits `0x150` before the entry in all four.

## Answers to the three questions

**Stack vs register.** Stack. Plain stdcall, `mov esi,[esp+8]` after one
push, `ret 4`. Matches XbSDB `STACK(default)` / one `PARAM(psh, unknown1)`.
No register ABI, no `ecx`/`edx` inputs.

**What `unknown1` is.** It is **`Count`, a pushbuffer slot count in
dwords** — the number of 32-bit words the caller is about to write. It is
scaled by 4 everywhere it appears (`lea edx,[eax+esi*4]`,
`lea ecx,[esi*4+0x204]`). The XbSDB `TODO: Update unknown parameter` at
`D3D8_OOVPA.c:234` is answered: the parameter is `Count`.

That file is vendored third-party, so the name is recorded here rather than
patched upstream-side; anyone syncing XbSDB can carry the rename with this
note as evidence. Our own binding must call the argument `Count`.

**Are the four bodies the same family.** Yes — identical instruction
sequence, one function, one XDK revision family.

**What Plume would have to do instead of writing NV2A.** Nothing. The
function does not write a single NV2A method. It is pure *capacity
reservation*: compare the write cursor against the limit and, only if
short, call `MakeRequestedSpace` to grow/flush the buffer. All actual GPU
mutation happens afterwards, when the caller writes its `Count` dwords into
the returned pushbuffer region.

## Go / no-go

**No-go as a standalone REPLACE.** It is a push-buffer writer's prologue and
belongs with the BeginPush family, for two reasons:

1. It is inseparable from what follows. Reserving space is meaningless
   unless the same commit also decodes the `Count` dwords the caller then
   writes. Binding this alone would swallow the reservation and leave the
   subsequent guest writes going to a buffer Plume does not read — Forza
   attaches and draws nothing/garbage, exactly the failure mode we agreed
   is worse than refusing.
2. Its callee `MakeRequestedSpace` is already wrapped for PGR2
   (`0x001C6140` → `d3d_hle_device_make_space` in the exact table), sitting
   immediately before it in the same file. The shared automatic path needs
   the same treatment, and both are part of the push machinery, not the
   deferred-state machinery.

It is **not** implementable against the existing deferred state — it never
touches deferred/dirty state at all. `d3d_hle_deferred_state.c` is the wrong
home for it.

Also note: Cxbx-Reloaded has **no** `D3DDevice_BeginStateBig`
implementation. The only hits in that tree are its bundled XbSymbolDatabase
copy — i.e. a detector name, not a reference body. There is no upstream
implementation to port.

## Recommendation

Fold BeginStateBig into the BeginPush commit as the reservation half of one
push-buffer slice, with `MakeRequestedSpace` bound alongside it as part of
the same machine (PGR2's exact table already points `0x001C6140` at it).
Keep it refusing until then.

The remaining refuse set is one commit, not more recon: BeginPush (both
arities) / EndPush / KickPushBuffer / BeginStateBig / MakeRequestedSpace.
That set is what still blocks Forza, CoD BRO and Spider-Man 2; PGR2 and
Sega GT are waiting only on the reservation half of the same thing.
