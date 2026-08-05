#!/usr/bin/env python3
"""Reader for Xenia GPU command-stream traces (`.xtr`, format version 1).

WHY THIS EXISTS
---------------
Nothing in this repo could read a GPU stream. Findings 9 and 10 both end at
"needs the decoder": the B1/B1b determinism baseline was unmeasured, and phase 4
(the PM4 command processor) has no offline gate without one. This is that
decoder, and everything in `tools/xtr_*.py` is a thin CLI on top of it.

WHY A MODULE RATHER THAN A SCRIPT PER TOOL
------------------------------------------
Asura's Wrath's port grew six `.xtr` tools, each carrying its own copy-pasted
`step()`. That is six copies of one belief about the file format. When the belief
is wrong it is wrong in all six and gets fixed in one, and the surviving five
keep producing confident numbers. The format lives here once.

FILE LAYOUT (xenia src/xenia/gpu/trace_protocol.h)
--------------------------------------------------
    TraceHeader { u32 version; char sha[40]; u32 title_id }      48 bytes

then a flat sequence of commands. Command structs are little-endian *host*
structs; PM4 words and memory payloads inside them are raw guest memory and so
are **big-endian**. Mixing those two up is the single easiest way to get
plausible nonsense out of this format.

     id  name                 fixed part                      total size
      0  PrimaryBufferStart   {type, base_ptr, count}          12
      1  PrimaryBufferEnd     {type}                            4
      2  IndirectBufferStart  {type, base_ptr, count}          12
      3  IndirectBufferEnd    {type}                            4
      4  PacketStart          {type, base_ptr, count}          12 + count*4
      5  PacketEnd            {type}                            4
      6  MemoryRead           MemoryCommand                    20 + encoded_len
      7  MemoryWrite          MemoryCommand                    20 + encoded_len
      8  EdramSnapshot        {type, enc, encoded_len}         12 + encoded_len
      9  Event                {type, event_type}                8   (event 0 = swap)
     10  Registers            {type, first, count, bool, enc, len}  24 + len
     11  GammaRamp            {type, rw, enc, len}             16 + len

    MemoryCommand = {u32 type, base_ptr, encoding, encoded_len, decoded_len}
    encoding: 0 = raw, 1 = snappy in **raw block format**
              (cramjam.snappy.decompress_raw — NOT the framed format that
              `snappy.decompress` expects).

`count` on PacketStart is a count of **dwords**, not bytes.

THE 2 GiB CLIFF IS FIXED, SO A DESYNC HERE IS A REAL FINDING
------------------------------------------------------------
Both template ports had to treat stream corruption as normal: Xenia's
`trace_writer.cc` took `long header_position = std::ftell(file_)` on the four
compressed-write paths, and `long` is 32-bit on Windows, so past 2 GiB the
seek-back-to-patch-the-header wrote to a wrapped offset and clobbered the file.
The operator fixed that at source for Case Zero (`xe::filesystem::Tell/Seek`,
64-bit) and rebuilt, which is how B2 exists at 7.95 GiB.

The consequence for this decoder is a change of *interpretation*, not of code:
Asura's Wrath expected desyncs and resynced past them quietly. Here a desync
means either a genuinely corrupt capture or — far more likely — that this file's
understanding of the format is wrong. So the walker still resyncs (it must, or
one bad byte discards the rest of an 8 GiB file), but it reports desyncs
**loudly, as regions with byte counts**, and never as a per-byte tally.

That last distinction matters more than it looks. Resyncing byte-by-byte and
counting one "desync" per rejected byte turns a single 4 KB corrupt region into
"4096 desyncs", which reads as a catastrophically broken capture. Here a run of
skipped bytes is one event that knows how wide it was.

THE PLAUSIBILITY LIMITS ARE HEURISTICS AND CAN LIE IN BOTH DIRECTIONS
---------------------------------------------------------------------
`step()` rejects a command whose length fields are absurd, because that is the
only way to notice that the walk has fallen out of alignment — the format has no
magic numbers or checksums. Those bounds are guesses about how large a real
field gets. Too tight and valid data reads as corruption; too loose and a desync
walks for megabytes before it is noticed. `--limits` prints the largest value
actually observed for each bounded field, so the guesses can be checked against
a real capture rather than left as folklore.
"""

import mmap
import struct

# --- command ids -----------------------------------------------------------
CMD_PRIMARY_BUFFER_START = 0
CMD_PRIMARY_BUFFER_END = 1
CMD_INDIRECT_BUFFER_START = 2
CMD_INDIRECT_BUFFER_END = 3
CMD_PACKET_START = 4
CMD_PACKET_END = 5
CMD_MEMORY_READ = 6
CMD_MEMORY_WRITE = 7
CMD_EDRAM_SNAPSHOT = 8
CMD_EVENT = 9
CMD_REGISTERS = 10
CMD_GAMMA_RAMP = 11

CMD_NAMES = ["PrimaryBufferStart", "PrimaryBufferEnd", "IndirectBufferStart",
             "IndirectBufferEnd", "PacketStart", "PacketEnd", "MemoryRead",
             "MemoryWrite", "EdramSnapshot", "Event", "Registers", "GammaRamp"]

MAX_CMD = 11

EVENT_SWAP = 0

HEADER_SIZE = 48

# Upper bounds used to detect that the walk has lost alignment. See the module
# docstring: these are heuristics, and `xtr_walk.py --limits` measures how much
# headroom a real capture leaves.
LIMIT_PACKET_DWORDS = 0x100000     # PacketStart.count
LIMIT_MEMORY_BYTES = 0x8000000     # Memory{Read,Write}.encoded_len (128 MB)
LIMIT_EDRAM_BYTES = 0x2000000      # EdramSnapshot.encoded_len (32 MB)
LIMIT_REGISTER_INDEX = 0x8000      # Registers.first / .count
LIMIT_REGISTER_BYTES = 0x1000000   # Registers.encoded_len (16 MB)
LIMIT_GAMMA_BYTES = 0x100000       # GammaRamp.encoded_len (1 MB)

# Precompiled: this is the hot loop of an 8.5 GiB walk.
_U32 = struct.Struct("<I")
_U32X2 = struct.Struct("<II")
_U32X3 = struct.Struct("<III")
_U32X5 = struct.Struct("<IIIII")
_HEADER = struct.Struct("<I40sI")
_BE_U32 = struct.Struct(">I")


class TraceError(Exception):
    pass


def open_trace(path):
    """mmap a trace and parse its header. Returns (data, header_dict).

    mmap rather than incremental reads: the whole point of the fixed cliff is
    that these files are now multi-gigabyte, and on 64-bit Linux mmap costs
    address space rather than RAM while letting the walk index arbitrarily.
    """
    f = open(path, "rb")
    data = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    if len(data) < HEADER_SIZE:
        raise TraceError(f"{path}: {len(data)} bytes — shorter than the 48-byte header")
    version, sha, title = _HEADER.unpack_from(data, 0)
    if version != 1:
        raise TraceError(f"{path}: trace format version {version}, this reader "
                         f"implements version 1")
    return data, {"version": version,
                  "sha": sha.rstrip(b"\0").decode("ascii", "replace"),
                  "title": f"{title:08X}",
                  "size": len(data)}


def step(data, off, n):
    """Offset of the next command, or a negative sentinel.

    Returns  >= 0  the next command's offset
             -1    this is not a plausible command (the walk has desynced)
             -2    no complete command here — end of file, or a truncated tail

    EVERY branch bounds-checks the command's FULL extent, payload included, not
    just its fixed-size header. That is not defensive padding; it is a bug this
    reader shipped with and B1b caught on its very first run. These captures end
    with a partially written command — the notes say GPU-trace shutdown preempts
    the final flush — so the last PacketStart in B1b sits 12 bytes from EOF with
    its payload missing. A header-only check accepts it, `walk` yields it, and
    the caller reads its first PM4 word off the end of the mmap and dies with a
    struct error naming an offset and no cause.

    Asura's Wrath's version of this function has the same hole (it returns
    `off + 12 + count*4` unchecked). It never fired there, which is worth
    stating plainly: an unexercised bounds check is not a working one.

    Order of the branches is by observed frequency, not by command id — this is
    called once per command over billions of bytes.
    """
    if off + 4 > n:
        return -2
    t = _U32.unpack_from(data, off)[0]

    if t == CMD_PACKET_START:
        if off + 12 > n:
            return -2
        count = _U32.unpack_from(data, off + 8)[0]
        if count > LIMIT_PACKET_DWORDS:
            return -1
        end = off + 12 + count * 4
        return -2 if end > n else end
    if t == CMD_MEMORY_READ or t == CMD_MEMORY_WRITE:
        if off + 20 > n:
            return -2
        enc, elen = _U32X2.unpack_from(data, off + 8)
        if enc > 1 or elen > LIMIT_MEMORY_BYTES:
            return -1
        end = off + 20 + elen
        return -2 if end > n else end
    if t == CMD_PACKET_END or t == CMD_PRIMARY_BUFFER_END or t == CMD_INDIRECT_BUFFER_END:
        return off + 4
    if t == CMD_PRIMARY_BUFFER_START or t == CMD_INDIRECT_BUFFER_START:
        return -2 if off + 12 > n else off + 12
    if t == CMD_EVENT:
        if off + 8 > n:
            return -2
        # The only event type Xenia writes is 0 (swap). A nonzero value here is
        # the cheapest available signal that we are reading a random dword.
        if _U32.unpack_from(data, off + 4)[0] != EVENT_SWAP:
            return -1
        return off + 8
    if t == CMD_REGISTERS:
        if off + 24 > n:
            return -2
        first, count, _cb, enc, elen = _U32X5.unpack_from(data, off + 4)
        if (enc > 1 or first > LIMIT_REGISTER_INDEX or count > LIMIT_REGISTER_INDEX
                or elen > LIMIT_REGISTER_BYTES):
            return -1
        end = off + 24 + elen
        return -2 if end > n else end
    if t == CMD_EDRAM_SNAPSHOT:
        if off + 12 > n:
            return -2
        enc, elen = _U32X2.unpack_from(data, off + 4)
        if enc > 1 or elen > LIMIT_EDRAM_BYTES:
            return -1
        end = off + 12 + elen
        return -2 if end > n else end
    if t == CMD_GAMMA_RAMP:
        if off + 16 > n:
            return -2
        _rw, enc, elen = _U32X3.unpack_from(data, off + 4)
        if enc > 1 or elen > LIMIT_GAMMA_BYTES:
            return -1
        end = off + 16 + elen
        return -2 if end > n else end
    return -1


def synced(data, off, n, need=200):
    """True if `need` consecutive commands parse from `off`.

    The format carries no magic number, so "is this a command boundary" can only
    be answered probabilistically: 200 commands that all have plausible lengths
    and land exactly on each other is not something random bytes do. Used both
    to skip a clobbered file head and to find the far side of a corrupt region.
    """
    for _ in range(need):
        off = step(data, off, n)
        if off == -2:
            return True
        if off < 0:
            return False
    return True


def find_start(data, n, limit=0x200000):
    """First walkable offset. Normally HEADER_SIZE.

    A pre-fix Windows capture could have its head clobbered by a wrapped header
    write, so this scans forward for a sync point. On a post-fix capture this
    returns 48 immediately and any other answer is itself a finding.
    """
    if synced(data, HEADER_SIZE, n):
        return HEADER_SIZE
    off = HEADER_SIZE
    while off < min(limit, n) and not synced(data, off, n):
        off += 1
    return off


def walk(data, n, start=None, on_desync=None, on_tail=None):
    """Yield (offset, command_id) for every *complete* command in the trace.

    Resyncs over corruption rather than stopping, because one bad byte in an
    8 GiB capture must not discard the remaining 8 GiB — but calls `on_desync`
    with (start_offset, bytes_skipped) so the caller reports a region, once,
    with its width. Byte-by-byte tallies of "desyncs" are how a single small
    corrupt region gets reported as thousands of failures.

    `on_tail(offset, bytes_left)` fires once at the end if the file stops
    mid-command. That is normal for these captures rather than a defect: the
    B1/B1b/B2 notes record that GPU-trace shutdown preempts the final flush, so
    a few trailing bytes of a half-written command are expected. It is reported
    anyway, because "expected" and "silently discarded" are different things.
    """
    if start is None:
        start = find_start(data, n)
    off = start
    while True:
        nxt = step(data, off, n)
        if nxt == -2:
            if on_tail is not None and off < n:
                on_tail(off, n - off)
            return
        if nxt < 0:
            bad_at = off
            probe = off + 1
            found = None
            while probe < n:
                if synced(data, probe, n):
                    found = probe
                    break
                probe += 1
            if on_desync is not None:
                on_desync(bad_at, (found if found is not None else n) - bad_at)
            if found is None:
                return
            off = found
            continue
        yield off, _U32.unpack_from(data, off)[0]
        off = nxt


# --- payload accessors -----------------------------------------------------

def packet_words(data, off):
    """(base_ptr, [PM4 dwords]) for a PacketStart at `off`. Big-endian payload."""
    base_ptr, count = _U32X2.unpack_from(data, off + 4)
    words = list(struct.unpack_from(f">{count}I", data, off + 12)) if count else []
    return base_ptr, words


def packet_header_word(data, off):
    """First PM4 dword of a PacketStart, or None if the packet is empty.

    Split out from `packet_words` because the census only ever needs word 0, and
    decoding a 32k-dword indirect buffer to look at its first entry is the
    difference between a two-minute walk and a twenty-minute one.
    """
    count = _U32.unpack_from(data, off + 8)[0]
    if not count:
        return None
    return _BE_U32.unpack_from(data, off + 12)[0]


def packet_word_count(data, off):
    """PacketStart.count — the packet's true length in dwords, header included.

    This is the boundary the recording says hardware used, which makes it the only
    independent check we have on our own packet-length arithmetic (see
    tools/pm4_packet_lengths.py). Note PM4_SHORT_RECORDED below.
    """
    return _U32.unpack_from(data, off + 8)[0]


def memory_command(data, off):
    """(base_ptr, encoding, encoded_len, decoded_len, payload_offset)."""
    base_ptr, enc, elen, dlen = struct.unpack_from("<IIII", data, off + 4)
    return base_ptr, enc, elen, dlen, off + 20


def decode_payload(data, payload_off, encoding, encoded_len, decoded_len):
    """Raw bytes of a compressed-or-not payload.

    encoding 1 is snappy in **raw block** format. `python-snappy`'s
    `decompress()` expects the *framed* format and fails here; cramjam's
    `decompress_raw` is the right call and is what both template ports use.
    """
    raw = data[payload_off:payload_off + encoded_len]
    if encoding == 0:
        return bytes(raw)
    import cramjam
    return bytes(cramjam.snappy.decompress_raw(raw))


def registers_command(data, off):
    """(first, count, contains_bool, encoding, encoded_len, payload_offset)."""
    first, count, cb, enc, elen = _U32X5.unpack_from(data, off + 4)
    return first, count, cb, enc, elen, off + 24


# --- PM4 -------------------------------------------------------------------

def pm4_type(word0):
    return word0 >> 30


def pm4_type3_opcode(word0):
    return (word0 >> 8) & 0x7F


def pm4_type3_count(word0):
    """Number of data dwords following a type-3 header (the field is count-1)."""
    return ((word0 >> 16) & 0x3FFF) + 1


# Xenos type-3 opcode names. Cross-checked against Fable 2's runtime/gpu/pm4.cpp
# Type3Name(), itself derived from Xenia's src/xenia/gpu/xenos.h.
#
# Two entries in this table were wrong on Asura's Wrath's first pass, and both
# failure modes are worth carrying forward because each reads as a discovery
# rather than as a bug:
#   * SET_BIN_MASK / SET_BIN_SELECT are 0x50/0x51 on Xenos (0x39/0x3A is the
#     Adreno numbering). The wrong entries were simply never hit, so the right
#     packets showed up as "<unknown>" — a made-up mystery.
#   * INTERRUPT is 0x54 on Xenos, not the Adreno 0x40. The count settles it: a
#     per-frame graphics interrupt fires once per frame, so the true opcode's
#     count tracks the swap count and the false one is absent entirely.
# An unknown opcode is therefore always printed by number, never bucketed as
# "other" — a packet we cannot name is exactly the thing phase 4 needs to see.
PM4_NAMES = {
    0x10: "NOP",
    0x21: "REG_RMW",
    0x22: "DRAW_INDX",
    0x23: "VIZ_QUERY",
    0x25: "SET_STATE",
    0x26: "WAIT_FOR_IDLE",
    0x27: "IM_LOAD",
    0x2B: "IM_LOAD_IMMEDIATE",
    0x2C: "IM_STORE",
    0x2D: "SET_CONSTANT",
    0x2E: "LOAD_CONSTANT_CONTEXT",
    0x2F: "LOAD_ALU_CONSTANT",
    0x34: "DRAW_INDX_BIN",
    0x35: "DRAW_INDX_2_BIN",
    0x36: "DRAW_INDX_2",
    0x37: "INDIRECT_BUFFER_PFD",
    0x3B: "INVALIDATE_STATE",
    0x3C: "WAIT_REG_MEM",
    0x3D: "MEM_WRITE",
    0x3E: "REG_TO_MEM",
    0x3F: "INDIRECT_BUFFER",
    0x44: "COND_EXEC",
    0x45: "COND_WRITE",
    0x46: "EVENT_WRITE",
    0x48: "ME_INIT",
    0x4A: "SET_SHADER_BASES",
    0x4B: "SET_BIN_BASE_OFFSET",
    0x4F: "MEM_WRITE_CNTR",
    0x50: "SET_BIN_MASK",
    0x51: "SET_BIN_SELECT",
    0x52: "WAIT_REG_EQ",
    0x53: "WAIT_REG_GTE",
    0x54: "INTERRUPT",
    0x55: "SET_CONSTANT2",
    0x56: "SET_SHADER_CONSTANTS",
    0x58: "EVENT_WRITE_SHD",
    0x59: "EVENT_WRITE_CFL",
    0x5A: "EVENT_WRITE_EXT",
    0x5B: "EVENT_WRITE_ZPD",
    0x5C: "WAIT_UNTIL_READ",
    0x5D: "WAIT_IB_PFD_COMPLETE",
    0x5E: "CONTEXT_UPDATE",
    0x60: "SET_BIN_MASK_LO",
    0x61: "SET_BIN_MASK_HI",
    0x62: "SET_BIN_SELECT_LO",
    0x63: "SET_BIN_SELECT_HI",
    0x64: "XE_SWAP",
}

DRAW_OPCODES = {0x22, 0x34, 0x35, 0x36}

# INDIRECT_BUFFER is recorded ONE DWORD SHORT, and phase 4 has to know it.
#
# Measured on B1, 2026-08-04: every one of its 28,726 INDIRECT_BUFFER packets is
# stored with PacketStart.count == 2 while the packet's own PM4 header says 3
# (header + address + size). No other opcode disagrees — 8,254,596 of them match
# exactly — so this is a property of how Xenia records the packet, not a wrong
# bit layout on our side. The split is perfectly categorical: 100% of 0x3F
# mismatch, 0% of everything else.
#
# The mechanism, also measured, all 28,726 with no exceptions:
#   * the very next trace command is always IndirectBufferStart, and
#   * that command's base_ptr equals the packet's word[1] (the IB address).
# The IB's contents then follow inline as ordinary commands until
# IndirectBufferEnd, so the trace never needs the size dword: a reader walks
# *into* the buffer rather than dereferencing guest memory at the address.
#
# CONSEQUENCE FOR PHASE 4. A replay tool that rebuilds a PM4 stream from a trace
# must special-case this. Feeding a command processor the recorded 2 dwords as
# if they were a whole packet hands it a malformed INDIRECT_BUFFER; trusting the
# header's 3 and reading a third dword out of the trace reads the *next
# command's* type field as an IB size. Both produce a plausible-looking stream
# that is wrong, which is the expensive kind of wrong.
PM4_SHORT_RECORDED = {0x3F: 1}   # opcode -> dwords omitted from PacketStart.count


def pm4_name(op):
    return PM4_NAMES.get(op, f"<unknown 0x{op:02X}>")
