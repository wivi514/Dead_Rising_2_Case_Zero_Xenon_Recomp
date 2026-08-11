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

## Retraction (part 27): the name table is NOT fixed-width outside the shader banks

The section above says names are "fixed-width entries, NUL-padded... 12 bytes wide in the
shader banks; the width is not in the header, it is `(data_start - names_offset) /
entry_count`, so compute it rather than hardcoding 12". Computing rather than hardcoding
was right and did not go far enough: **the width is not a property of the format at all.**

`tools/big_list.py` enforced that divisibility as a parse check on its first run and **95
of 146 archives failed it** — `charvocals.big` has 9,843 bytes of names over 985 entries,
`datafile.big` 686 over 37. Their names are simply variable-length.

**Read to the NUL from each entry's own `name_offset`.** That handles both: the shader
banks are the case where every name happens to be padded to a common stride, and nothing
needs to know what that stride is. With the change all 146 archives parse and yield
**12,481 entries**.

The original claim was generalised from the seven shader banks, which is where the format
was cracked — the same shape as the 40-byte-stride error the section above already
records, one level up. **A structural constant derived from one family of files in a
container is a property of that family until a second family says otherwise.**

## Retraction 2 (part 27): entries ARE compressed, and `size2` is the uncompressed length

The Layout section says of the index entry's `size`/`size2` pair:

> `size` and `size2` being equal everywhere is the signature of an
> uncompressed-but-compressible container... **Do not assume they are always equal** — a
> loader that reads only one of them will work on Case Zero and break on the first archive
> that compresses anything.

**The warning was right and the observation was wrong, in Case Zero itself.** The original
survey was the seven shader banks, which store everything. Over all 146 archives:

| | entries |
|---|---|
| stored (`size == size2`) | 10,810 |
| **COMPRESSED (`size != size2`)** | **1,671** |

in 9 archives — `streamedassets.big` (1110 of 1110), `models/npcs.big` (185), `cine_props.big`
(91), `preload4.big` (86), `frontend/ingame.big` (80), `frontend/fecmn.big` (62),
`datafile.big` (37), `models/zombies.big` (12). **`size` is the STORED length and `size2`
is the UNCOMPRESSED length.**

### The compressed stream

Parsed exactly on `cc_03.bct` — 71,580 of 71,580 bytes consumed, five chunks, nothing left
over, which is the check that a guessed layout is right:

```
0x00   4   uncompressed size, BIG-endian   (131,120 — equals the entry's size2)
0x04   4   window size, BIG-endian         (0x8000 = 32 KB)
then, repeated until the entry ends:
       4   compressed chunk length, BIG-endian
       n   the chunk, which opens 0xFF — an Xbox LZX / XMemCompress block
```

Note the ENDIANNESS FLIP: the container's header and index are little-endian (the Layout
section says so twice), and the compressed stream inside an entry is big-endian. Both are
true at once and a reader that picks one for the whole file gets a plausible-looking size
and nonsense chunks.

**Decompression is not implemented.** `tools/big_list.py --extract` writes the compressed
stream and SAYS that is what it wrote, rather than producing a file that looks like a
`.bct`, is named like one, and is not one.

### What `cc_03.bct` turned out to be

`size2` = **131,120 = a 48-byte `.bct` header + 32*32*32*4**, i.e. a **32-cubed RGBA
colour-correction LUT**, and there are 15 of them (`cc_01`..`cc_15`) in
`streamedassets.big`. Relevant to open item 6: part 25 established that **zero** shaders in
our cache sample descriptor set 1 (`Texture3D`), so if the title uses these it must unroll
the cube into a 2D strip — which is the usual 360 technique and is consistent with both
facts at once.

### Decompressed, and what `cc_03.bct` actually contains

`tools/big_decompress.cpp` links XenonRecomp's own `lzxDecompress` — the one the
recompiler uses on this title's XEX — rather than vendoring a second decoder, and checks
its output against an oracle instead of asking anyone to eyeball it: every loose `.bct` on
disc begins `05 01 01 E2`, so the tool tries both plausible chunk framings and accepts
only the one whose output carries that magic at the entry's declared `size2`. The
per-chunk framing wins: each chunk opens `0xFF`, then a BE u16 uncompressed length and a
BE u16 compressed length, then an independent LZX stream.

The `.bct` header's bytes 4..7 are the extent as two BE u16: `04 00 00 20` = **1024 x 32**,
and 1024*32*4 = 131,072 = the payload. So the 32-cubed LUT is stored **unrolled into a
1024x32 2D strip** — which is why part 25's census found zero shaders sampling descriptor
set 1 (`Texture3D`) and both facts are true at once.

**AND IT IS TILED.** Read linearly the LUT's neutral diagonal is not monotone under ANY of
the six axis assignments (17 of 31 steps, all six identical). Untiled with the runtime's
own `Tiled2DOffset`, every one of them reads **31 of 31**. A shipped 360 texture is tiled;
that is obvious in hindsight and was not obvious while the numbers looked merely noisy.

**The axis order needs an ASYMMETRIC probe.** After untiling all six assignments score
31/31, because the neutral ramp `r = g = b` is symmetric under permuting the axes — the
test that proved the untiling cannot settle the layout. The PRIMARIES do: only
`x = r + 32b, y = g` sends red to red, green to green and blue to blue.

`cc_03` is then readable as a grade:

| in | out |
|---|---|
| black | 0, 0, 0 |
| mid grey (123) | R76 G91 B117 |
| white | 255, 255, 255 |
| pure red | R147 G70 B78 |
| pure green | R32 G168 B89 |
| pure blue | R19 G27 B102 |

Endpoints preserved, midtones pulled down hard (R -48, G -33, B -5 at the middle) with the
blue lifted above identity in the highlights, and the primaries desaturated — a cool,
crushed-midtone night grade.
