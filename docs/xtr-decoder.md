# The Xenia `.xtr` GPU trace format, and how to read one

Written 2026-08-04 (session 3), when the first `.xtr` decoder in this workspace was
built. Findings 9 and 10 both ended at *"needs the decoder"*: the B1/B1b determinism
baseline was unmeasured and phase 4 had no offline gate. This document is the format,
the traps, and the two things Case Zero's captures taught us that were not in either
template port's notes.

Written for someone porting a **different** Xbox 360 title: nothing below is specific to
Dead Rising 2 except the measured numbers, which are labelled as such.

## Why you want this at all

A `.xtr` is the GPU's side of a run: every PM4 packet the guest submitted, every register
write, every block of memory the command processor read, in order, with frame boundaries
marked. It is the only ground truth that says what a renderer is *supposed* to do, and it
is capturable without a debugger. Phase 4's gate — replay the stream offline, report zero
unknown opcodes and zero desyncs — is built entirely on it.

## File layout

```
TraceHeader { u32 version; char sha[40]; u32 title_id }        48 bytes
```

then a flat sequence of commands, each starting with a `u32` type:

| id | name | fixed part | total size |
|---|---|---|---|
| 0 | PrimaryBufferStart | `{type, base_ptr, count}` | 12 |
| 1 | PrimaryBufferEnd | `{type}` | 4 |
| 2 | IndirectBufferStart | `{type, base_ptr, count}` | 12 |
| 3 | IndirectBufferEnd | `{type}` | 4 |
| 4 | PacketStart | `{type, base_ptr, count}` | 12 + count*4 |
| 5 | PacketEnd | `{type}` | 4 |
| 6 | MemoryRead | MemoryCommand | 20 + encoded_len |
| 7 | MemoryWrite | MemoryCommand | 20 + encoded_len |
| 8 | EdramSnapshot | `{type, enc, encoded_len}` | 12 + encoded_len |
| 9 | Event | `{type, event_type}` | 8 |
| 10 | Registers | `{type, first, count, bool, enc, len}` | 24 + len |
| 11 | GammaRamp | `{type, rw, enc, len}` | 16 + len |

`MemoryCommand = {u32 type, base_ptr, encoding, encoded_len, decoded_len}`.
`PacketStart.count` counts **dwords**, not bytes. `Event` type 0 is a swap, i.e. a frame
boundary — it is the only event type Xenia writes, which makes a nonzero value there a
useful cheap signal that you are reading a random dword.

Source of truth: `xenia/src/xenia/gpu/trace_protocol.h`.

### The endianness trap

Command structs are little-endian **host** structs. PM4 words and memory payloads inside
them are raw guest memory and are **big-endian**. Getting this backwards does not throw —
it yields plausible nonsense, because a byte-swapped PM4 header still has a type field and
still selects *some* opcode. Every number downstream then looks reasonable and is wrong.

### Compression

`encoding` 1 is snappy in **raw block** format. `python-snappy`'s `decompress()` expects
the *framed* format and fails; use `cramjam.snappy.decompress_raw`. `encoding` 0 is raw.

## The 2 GiB cliff, and why it no longer applies

Both template ports treated stream corruption past 2 GiB as a fact of life.
`trace_writer.cc` took `long header_position = std::ftell(file_)` on its four
compressed-write paths, and `long` is 32-bit on Windows, so past 2 GiB the
seek-back-to-patch-the-header wrote to a wrapped offset and clobbered the file. The
operator found and fixed this at source for Case Zero (`xe::filesystem::Tell/Seek`, which
were already in the tree) and rebuilt — which is how B2 exists at 7.95 GiB, four times
past the old limit, intact.

**The format was never the limit.** Every command carries its own length; nothing stores an
absolute file offset. A 64-bit sequential reader — which Python is natively — handles an
8 GiB file with no format change at all.

The consequence for a decoder is one of *interpretation*, not code: a desync is no longer
expected background noise, so it must be reported loudly. Ours does, and it reports
**regions with byte widths**, never a per-byte tally. Resyncing byte-by-byte and counting
one "desync" per rejected byte turns a single 4 KB corrupt region into "4096 desyncs",
which reads as a catastrophically broken capture.

## Trap 1: a length field you cannot bounds-check is a crash waiting for the right file

Asura's Wrath's `step()` computes `off + 12 + count*4` and returns it without checking it
against the file size. Ours was ported with the same hole, and **B1b found it on the first
run**: these captures stop mid-command, because GPU-trace shutdown preempts the final
flush. B1b's last `PacketStart` header sits 12 bytes from EOF with its payload missing. A
header-only bounds check accepts it, the walker yields it, and the caller reads the first
PM4 word off the end of the mmap — dying with a `struct.error` that names an offset and no
cause.

B1 never triggered it. That is the point worth carrying: **an unexercised bounds check is
not a working one**, and "it has always worked" is a statement about the inputs, not the
code. Bounds-check the command's *full extent*, payload included, in every branch, and
report a truncated tail as its own outcome rather than a corruption.

## Trap 2: one opcode can be recorded differently, and the check that catches it is free

Two independent things in the stream encode the same packet length: the trace's
`PacketStart.count`, and the PM4 type-3 header's own count-1 field. If your bit layout is
wrong they disagree. Comparing them costs nothing and is the only check that can *fail* —
which is what makes it worth having. Everything else a census prints would look entirely
plausible with a wrong shift, because a wrong opcode is still some other real opcode.

On B1 it fired: 28,726 of 8,283,322 type-3 packets disagreed (0.35%). The shape of the
disagreement is what identifies it, and it is worth reading carefully because the two
possible shapes have opposite meanings:

- **Spread across many opcodes** → the bit layout is wrong and every count is suspect.
- **All of one opcode and none of any other** → that opcode is recorded differently.

Here it was perfectly categorical: **100% of `INDIRECT_BUFFER` (0x3F) mismatched, 0% of
every other opcode did.**

### What `INDIRECT_BUFFER` actually does

Measured across all 28,726, no exceptions:

- The packet is stored with `count = 2` while its PM4 header says 3
  (header + address + size). **The size dword is omitted.**
- The very next trace command is *always* `IndirectBufferStart`.
- That command's `base_ptr` always equals the packet's word[1], the IB address.
- The IB's contents then follow inline as ordinary commands until `IndirectBufferEnd`.

So the trace never needs the size: a reader walks *into* the buffer rather than
dereferencing guest memory at the address. The information is relocated, not lost.

**This is a phase 4 trap, not a curiosity.** A replay tool rebuilding a PM4 stream must
special-case it. Feed the command processor the recorded 2 dwords as a whole packet and it
gets a malformed `INDIRECT_BUFFER`; trust the header's 3 and read a third dword out of the
trace and you read *the next command's type field* as an IB size. Both produce a
plausible-looking stream that is wrong — the expensive kind.

Encoded as `xtr.PM4_SHORT_RECORDED`, so `--verify` stays silent on the known case and
still screams about anything else. **A check that always fires is a check people learn to
ignore**, so the right response to a known-benign alarm is to encode the knowledge, never
to widen the tolerance.

### The self-check was proven capable of failing

A test that has never failed has not been shown capable of failing, so the check was
deliberately broken two ways against B1b:

| injected fault | result |
|---|---|
| type-3 count field shifted 16→17 | **100%** mismatch, spread across every opcode |
| PM4 words read little-endian | **100%** mismatch; opcode table collapses to 4 entries, 3 unknown |

Both produce the *"spread across many opcodes"* signature, cleanly distinguishable from
`INDIRECT_BUFFER`'s real "one opcode, 0.35%". So the diagnostic rule above is not just
plausible — it separates the two cases in practice.

The endianness control is the more instructive one. Reading PM4 words little-endian did
not throw: it produced a census listing `LOAD_ALU_CONSTANT` 14 times and three unnamed
opcodes, which is exactly what "plausible nonsense" looks like. Without the length
cross-check, that output has nothing obviously wrong with it.

## Trap 3: the plausibility bounds are guesses, so measure them

The format has no magic number and no checksum, so "have I lost alignment" can only be
answered from whether the length fields look absurd. Those bounds are guesses. Too tight
reports a valid capture as corrupt; too loose lets a desync run for megabytes unnoticed.
`xtr_walk.py limits` prints the largest value each bounded field actually reaches, so the
headroom is a measured number rather than folklore inherited from another title.

## Trap 4: PM4 opcode tables carry Adreno numbers

Xenos and Adreno A2xx share a command-processor lineage and much of an opcode table, so a
table assembled from Adreno documentation is *mostly* right — which is worse than being
wrong. Asura's Wrath shipped two such errors:

- `SET_BIN_MASK`/`SET_BIN_SELECT` are **0x50/0x51** on Xenos, not 0x39/0x3A. The wrong
  entries were never hit, so the real packets appeared as `<unknown>` — a manufactured
  mystery.
- `INTERRUPT` is **0x54**, not the Adreno 0x40. The counts settle it: a per-frame graphics
  interrupt tracks the swap count, and 0x40 never appears at all.

An unnamed opcode must always be printed by number, never bucketed as "other". A packet
you cannot name is exactly what phase 4 needs to see; folding it into a tidy total
converts a gap in your knowledge into a number.

## Reading determinism from two captures

`.xtr` size is **not** a determinism metric, and a byte-diff is worse than useless. A
continuous stream emits frames for as long as the run lasts, so size mostly measures how
long the operator sat at a loading screen; and host fields (addresses, handles,
timestamps) differ every run under ASLR even under perfect guest determinism. B1 and B1b
are 1.61 GiB and 1.12 GiB — a ratio of 0.70 that means nothing.

The method that works, and the reasoning behind each choice:

1. **Fingerprint each frame by content**: command counts, packet-type counts, and the
   type-3 opcode histogram. Optionally an ordered hash of every type-3 header word, which
   is stricter without ever touching an address.
2. **Exclude addresses from the verdict** and report them as their own line. An address
   difference is a memory-layout question, not a rendering-content one; letting it into
   the verdict makes every run look non-deterministic.
3. **Align with a sequence matcher, not a fixed index.** Runs are deterministic in content
   but jittery in *phase* — one extra frame on a load shifts everything after it, and a
   naive frame-*i*-vs-frame-*i* comparison then reports near-total divergence for a pair
   of runs that rendered identically. `difflib.SequenceMatcher` absorbs insertions and
   deletions and reports where they happened.

`tools/xtr_determinism.py` prints the naive number *and* the aligned one, precisely so the
gap between them is visible rather than hidden behind whichever one the author preferred.

### The two traps that are *not* obvious, both of which we fell into

**Emulator bookkeeping in a content fingerprint.** `MemoryRead`/`MemoryWrite` are Xenia
recording guest memory so the trace replays standalone; whether a block needs recording
depends on the emulator's dirty-tracking, not the guest. On B1/B1b they align per-frame on
only **17.7%**, and folding them in dragged an otherwise-agreeing fingerprint from 42.7%
to 16.0%. The subtlety: the same counts agree to **0.37% in aggregate**. An emulator-side
field can be deterministic in total and distribute differently across frames — which
fails exactly the comparison you are most tempted to use it in.

**Comparing the whole run.** These captures end when a human presses exit. B1 sat on the
title for 619 frames, B1b for 409. That difference swamps everything real: 42.7%
whole-run against 80.0% over the prefix. Compare the fixed prefix; exclude the final era.
`xtr_determinism.py` does this by default and requires `--include-tail` to do otherwise.

### Measured baseline (Case Zero, B1 vs B1b, 2026-08-04)

Over the boot+movie prefix: **worst aggregate delta 0.42%, draws 0.19%**, four eras
agreeing to the individual draw. Frame-exact alignment only **80.0%**, with phase drift
concentrated at lag +3 (55.9% of matched frames). Asura's Wrath measured ~0.38% on the
same kind of comparison, so this looks like a property of the emulator and the method
rather than of either title.

**The operational conclusion is a design constraint on phase 4: gate on per-era
aggregates, never on absolute frame index.** A frame-indexed gate would report ~20%
divergence against a *correct* renderer. Full write-up: finding 10 in
`docs/xenia-capture-analysis.md`.

## Structural facts a replay parser needs

**Nesting is not balanced at the tail.** B1 has 24,527,474 `PacketStart` against
24,527,472 `PacketEnd`, and 2,333 `PrimaryBufferStart` against 2,332 ends. The stream
stops while two levels deep — an `INDIRECT_BUFFER` packet stays open while the IB's own
packets are emitted inside it. A parser that requires balanced nesting rejects every one
of these captures.

**Watch `Registers.count`.** Measured headroom on B1, via `xtr_walk.py limits`:

| field | observed max | limit | headroom |
|---|---|---|---|
| packet_dwords | 1,025 | 1,048,576 | 1,023× |
| memory_bytes | 914,357 | 134,217,728 | 147× |
| edram_bytes | 491,844 | 33,554,432 | 68× |
| register_count | **20,483** | **32,768** | **2×** |
| register_bytes | 3,866 | 16,777,216 | 4,340× |
| gamma_bytes | 2,565 | 1,048,576 | 409× |

`register_count` is the tight one at 2×, because that single command is a snapshot of the
whole Xenos register file (0x5003 entries) rather than something that scales with capture
length — so it should not grow. Everything else has three orders of magnitude of room.
This is the table to re-check on a title that reports a desync where none should exist.

## The capture is an oracle for your own parser, not just a record of the run

`PacketStart {base_ptr, count}` carries the packet's true length in dwords — the
boundary the hardware command processor used, for this title, inside indirect buffers
as well as the ring. That makes a capture the one independent check available on a
runtime's packet-length arithmetic, which is otherwise self-referential: a parser that
sizes a packet wrongly desyncs, reads data as headers, and stops somewhere downstream
looking like a corrupt stream rather than like a bug in itself.

`tools/pm4_packet_lengths.py` does that comparison. On B1: **24,527,474 packets
checked, zero disagreements.** Run it after any change to `gpu/pm4.cpp`'s packet decode.
Exit 1 means our walk would desync on a real stream.

Two caveats it has to encode, both from this file's own traps:

- `INDIRECT_BUFFER` is recorded one dword short (trap 2), so the comparison adds it
  back via `xtr.PM4_SHORT_RECORDED` rather than widening a tolerance.
- a zero-length `PacketStart` has no header to check and is skipped, not counted.

...and one it learned the hard way. B1 contains a single `PacketStart` whose header
records as `00000000`. Read as a type-0 register write it is a two-dword packet, and
phase 1 finding 38 used exactly that reading to settle how a command processor should
treat a zero dword. It is not one: its `count` is 2, its word[1] is a command-buffer
address, and the next trace command is an `IndirectBufferStart` with that base — every
marker of an `INDIRECT_BUFFER`, whose header the trace simply failed to record. (It
sits at `03D71FFC`, the last dword of a 4 KB block; the mechanism is not established.)
The tool now classifies packets by that mechanism rather than by header bits, so its
clean result stops being an artifact that agrees by luck — and the honest statement
about the capture is that **it contains no genuine zero-header packet and has no
opinion on the question.** One anomalous record deserves identifying before anything is
concluded from it.

### The walk is a separate question from the lengths

Every packet's length being right does not make a *walk* right: it says nothing about
the address the walk starts at, nor about whether the packets tile the buffer. Both
gates passed while our command processor desynced dozens of times a minute.

`tools/pm4_indirect_walks.py` closes the reachable half of that gap, and it works
because Xenia walks *into* each indirect buffer rather than dereferencing guest memory
— so every packet inside one is recorded with its own `base_ptr`, the address hardware
read it from. Chaining `base_ptr(i) + 4 * our_length(header(i)) == base_ptr(i+1)`
replays our cursor against every boundary hardware landed on, starting from the
`INDIRECT_BUFFER` packet's own address dword. On B1: **28,726 buffers, 0 disagreements.**

What it cannot check is the declared size: `IndirectBufferStart.count` is recorded as 0
for every buffer, and the size dword is the one the short recording drops. It prints
the size hardware *consumed* instead, so an implausible declared size would stand out.

And what neither tool can check is whether the bytes in guest memory are the bytes
hardware saw. That is where finding 39's defect actually lived.

## The tools

| tool | what it answers |
|---|---|
| `tools/xtr.py` | the format itself — a module, not a script |
| `tools/xtr_walk.py stats` | is this capture intact, and what is in it |
| `tools/xtr_walk.py index` | byte offset of every frame |
| `tools/xtr_walk.py limits` | how much headroom the plausibility bounds have |
| `tools/xtr_pm4_census.py` | what the guest told the GPU to do; `--verify` self-checks |
| `tools/xtr_determinism.py` | how much two captures of one drive differ |
| `tools/pm4_packet_lengths.py` | does OUR command processor size packets the way hardware did |
| `tools/pm4_indirect_walks.py` | does our WALK of an indirect buffer land where hardware's did |

The format lives in **one** module on purpose. Asura's Wrath grew six `.xtr` tools each
carrying a copy-pasted `step()` — six copies of one belief about the file format. When the
belief is wrong it is wrong in all six and gets fixed in one, and the surviving five keep
producing confident numbers. (The truncated-tail bug above is exactly that bug class,
caught here because there was only one copy to fix.)
