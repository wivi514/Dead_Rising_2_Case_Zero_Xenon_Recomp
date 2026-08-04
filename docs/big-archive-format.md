# The `.big` archive format (Blue Castle Games engine)

Cracked 2026-08-04 from the shipped archives plus the A5 read-seek oracle. Every asset
Case Zero loads comes out of one of these — 433 distinct archives over a gameplay
session — so the VFS and every loader phase depends on this.

This should transfer **verbatim to Dead Rising 2: Case West** (same engine) and probably
to the full Dead Rising 2.

## Layout

All integers are **little-endian**, which is worth stating twice: the guest is big-endian
PowerPC, the executable is big-endian, and the *archives are not*. A reader that assumes
big-endian throughout gets a plausible-looking magic (`0x06050403`) and nonsense
everywhere after.

```
offset  size  field
0x00     4    magic          bytes 06 05 04 03  (= 0x03040506 read LE)
0x04     4    data_start     file offset where the payload region begins
0x08     4    total_size     total file size (matches the file exactly)
0x0C     4    entry_count
0x10     4    header_size    0x18 in every archive seen
0x14     4    names_offset   start of the name table (== 0x18 + entry_count*28)
0x18   28*n   index          entry_count records, 28 bytes each
       ...    name table     names_offset .. data_start
data_start    payload
```

### Index entry (28 bytes)

```
offset  size  field
0x00     4    name_offset    absolute file offset into the name table
0x04     4    hash           name hash (the runtime looks entries up by this)
0x08     4    size
0x0C     4    size2          equal to `size` in every entry observed
0x10     4    data_offset    absolute file offset of the payload
0x14     4    flags          0x80 in every entry observed
0x18     4    reserved       0 in every entry observed
```

`size` and `size2` being equal everywhere is the signature of an
uncompressed-but-compressible container: the pair is almost certainly
(uncompressed, stored) and this build simply ships everything stored. **Do not assume
they are always equal** — a loader that reads only one of them will work on Case Zero and
break on the first archive that compresses anything.

### Name table

Fixed-width entries, NUL-padded. 12 bytes wide in the shader banks; **the width is not in
the header**, it is `(data_start - names_offset) / entry_count`, so compute it rather than
hardcoding 12. Names are lowercase, e.g. `a07a5e80.vo`.

## Verification

The self-consistency check that confirms a parse, on `deadrisingprologue-vs.big`:

```
0x18 + entry_count*28 == names_offset      0x18 + 143*28 = 0xFBC   ✓
total_size == actual file size             0x378FC = 227,580        ✓
(data_start - names_offset) % entry_count == 0                      ✓
```

An earlier parse of this file assumed a 40-byte stride, which happens to make
`0x18 + 143*40 == data_start` come out right for *this one archive* and wrong for the
other six. The stride is settled by `names_offset`, which is in the header — always use
it, never infer the stride from `data_start`.

## Independent confirmation from the A5 read log

A5 (`log_high_frequency_kernel_calls=true`) makes `NtReadFile` visible, and the observed
read pattern matches this layout directly. For one datafile-class archive:

```
len=0x800  off=0x0        <- header + start of index
len=0x800  off=0x0
len=0x6C000 off=0x0       <- whole index + name table
len=0xB000  ...           <- payload chunks
```

Small `0x800` probes at offset 0 first, then a large read covering the index region, then
payload — i.e. the runtime reads the header, sizes the index from it, reads the index,
and then seeks to individual `data_offset`s. That is the loader shape our VFS has to
support: **random access within an archive, not sequential streaming.**

Reads are keyed by **handle**, not filename, so grepping `.big` on a read line finds
nothing. The reconstruction is: per thread, zip each `NtCreateFile(name)` with the
`Added handle:H` line that follows it on the same thread, then attribute every
`NtReadFile(H, …)` to that name until its `NtClose(H)`. Recipe and field map:
`Xenia logs/A5_highfreq_boot/A5_NOTES.txt`.

## The shader banks specifically

`data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big` are `.big` archives whose
entries are named `<hash>.vo` — shader objects, not raw microcode.

**They are not a source of ready-to-use Xenos microcode.** An entry's payload carries
build metadata including the original build path, e.g.

```
c:\bcg\deadrisingprologue\intermediate\xbox360\shaders\a07a5e80.updb
```

(`.updb` = the Xbox 360 shader debug database.) Measured against the microcode Xenia
observes the guest actually submit, an entry shares **4 of 159** aligned 8-byte n-grams —
and two *unrelated* dumped shaders share 4 of 73, so that is background noise from common
instruction encodings, not a relationship. Payload entropy is 5.03, so this is not a
compression artifact; the bytes simply are not the same bytes.

**This does not block the renderer.** Xenia's `dump_shaders` yields the real thing:
455 distinct raw Xenos microcode blobs (120 frontend/menu from A1, 335 gameplay from A2)
as `*.ucode.bin.{vert,frag}`, with disassembly alongside. That is exactly XenosRecomp's
input. See `docs/xenia-capture-analysis.md` finding 6.

## Open

- The relationship between a `.vo` payload and the submitted microcode. Probably a
  container with the microcode in a pre-fixup form, but unproven, and **not on the
  critical path** now that ground-truth microcode is in hand.
- What `sc` / `sd` / `ss` hold (`vs`/`vd`/`ps`/`pd` are evidently vertex/pixel shader and
  declaration banks).
- Whether any archive in the game ships compressed payloads (`size != size2`).
