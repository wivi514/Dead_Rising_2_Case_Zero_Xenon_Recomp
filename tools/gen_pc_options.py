#!/usr/bin/env python3
"""Generate the patched frontend assets that wake the title's own PC graphics menu.

AS OF THE GITHUB RELEASE (release-github-plan §0) THIS TOOL IS THE REFERENCE, NOT
THE ROAD: runtime/host/overlay_gen.cpp is a line-for-line C++ port that runs the
same transforms at a player's first run (a shipped bundle has no Python and must
not ship the Capcom-derived outputs), and its contract is BYTE IDENTITY with this
file — `cz_runtime --gen-overlays` + `diff -r` is the gate, and the clean-container
gate hashes the container-generated fecmn.big against this tool's output. If you
change ANY transform here, port the change and bump overlay_gen.cpp's
kGeneratorVersion in the same commit.

WHY THIS EXISTS (part 60). The 360 XEX ships a complete PC options screen —
data/frontend/fecmn.big carries options_pc.txt (Resolution, DisplayMode, VSync,
Shadow, Multisampling, ...), the path manifests wire `OptionsPC` into both menu
graphs, and str_en.bcs resolves every label — but the screen's verb HANDLERS were
compiled out of the 360 build, and the PC layout expects a runtime to populate its
value texts (its Resolution CTSelect literally reads "System.Collections.ArrayList",
a leftover of the PC tool that filled it). So the shipped layout cannot work as-is.

This script produces, into assets/game_patched/data/frontend/ (gitignored, like all
game data — the transform is code, the content stays Capcom's):

  * fecmn.big — a full repack whose options_pc.txt is rewritten to the WORKING 360
    spinner idiom (options_gameplay.txt is the model): each kept row is a
    cFESpinGroup whose CTSelect is a cFETextList with the value strings baked in,
    Loop="true", and ACT:Prev:<Name>/ACT:Next:<Name> verbs. The runtime's hook
    (runtime/cpu/pc_options.cpp) relays those verbs to the spin widget exactly the
    way the title's own screens do, and applies the result host-side. Rows whose
    setting we do not implement are REMOVED, not left dead — a menu item that does
    nothing is the gamma slider all over again (part60-kickoff §3).
  * str_*.bcs — every language bank, with the handful of value strings the 360 bank
    does not have (resolution names, "Borderless") added under ids 60000+. The ids
    were chosen far above the shipped maximum (~32k) so a future title update could
    never collide - CORRECTED: the first pick, 60000+, collided immediately; see NEW_STRINGS.

The VFS serves these in preference to assets/game/ (kernel/vfs.cpp overlay;
CZ_NO_PATCHED_ASSETS=1 is the control arm that restores the shipped data).

Verification is built in: the repacked archive is re-parsed and every entry's
payload compared byte-for-byte against the original (the rewritten one against its
new text), and the patched string banks are re-read through the same reader the
analysis used, checking both every original id and every added one.

Usage:
    python3 tools/gen_pc_options.py            # writes assets/game_patched/...
    python3 tools/gen_pc_options.py --print    # show the rewritten options_pc.txt
"""

import argparse
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRONTEND = os.path.join(REPO, 'assets/game/data/frontend')
OUT_FRONTEND = os.path.join(REPO, 'assets/game_patched/data/frontend')

# ---------------------------------------------------------------------------
# .big reading (the format is docs/big-archive-format.md; LZX per-chunk streams
# are decompressed via tools/big_decompress, but this script never needs to: the
# rewritten entry is generated from big_decompress's own output checked in at
# generation time, and every other entry is carried as its stored bytes.)
# ---------------------------------------------------------------------------

ENT = struct.Struct('<7I')  # name_off, hash, size, size2, data_off, flags, resv


def read_big(path):
    raw = open(path, 'rb').read()
    magic, data_start, total, count, hdr, names_off = struct.unpack_from('<6I', raw, 0)
    assert magic == 0x03040506, f'{path}: bad magic {magic:#x}'
    assert hdr == 0x18 and names_off == 0x18 + count * 28
    entries = []
    for i in range(count):
        no, h, sz, sz2, off, flags, resv = ENT.unpack_from(raw, 0x18 + i * 28)
        end = raw.index(b'\0', no)
        entries.append({
            'name': raw[no:end].decode('ascii'), 'name_off': no, 'hash': h,
            'size': sz, 'size2': sz2, 'data_off': off, 'flags': flags, 'resv': resv,
            'stored': raw[off:off + sz],
        })
    return raw, data_start, names_off, entries


def write_big(path, raw_orig, data_start, names_off, entries, align=4):
    """Rebuild the archive: header + index + the ORIGINAL name table bytes +
    payloads packed in original file order. `align` preserves the source
    archive's own placement granularity — fecmn.big packs at 4 bytes, while
    preload4.big places every payload 0x800-aligned (the A5 read oracle shows
    0x800-granular I/O, so a loader assumption about that alignment is cheap to
    respect and expensive to discover)."""
    name_table = raw_orig[names_off:data_start]
    order = sorted(range(len(entries)), key=lambda i: entries[i]['data_off'])
    pos = data_start
    payload = bytearray()
    for i in order:
        e = entries[i]
        pad = (-pos) % align
        payload += b'\0' * pad
        pos += pad
        e['data_off'] = pos
        payload += e['stored']
        pos += len(e['stored'])
        e['size'] = len(e['stored'])
    total = pos
    out = bytearray()
    out += struct.pack('<6I', 0x03040506, data_start, total, len(entries), 0x18,
                       names_off)
    for e in entries:
        out += ENT.pack(e['name_off'], e['hash'], e['size'], e['size2'],
                        e['data_off'], e['flags'], e['resv'])
    out += name_table
    out += payload
    assert len(out) == total
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(bytes(out))


def fake_lzx_stream(data):
    """An XMemCompress-framed stream of LZX VERBATIM blocks that carry every byte
    as a literal — a conforming LZX stream written without implementing a
    compressor's search.

    WHY VERBATIM AND NOT THE UNCOMPRESSED BLOCK TYPE: the first attempt used LZX
    block type 3 (raw copy). libmspack's decoder accepted it; the GUEST's did not —
    the boot died identically with our re-encode of the ORIGINAL bytes, which
    exonerated the content and convicted the stream. A minimal 360-era decoder
    plausibly never implemented the block type its own encoder never emitted.
    Verbatim blocks are the type every shipped chunk uses, so this stays on the
    decoder path the title exercises 1,671 times per boot.

    THE TRICK: a main Huffman tree whose 256 literal symbols all have code length
    8 is canonically complete, and canonical assignment then maps symbol i to code
    i — every payload byte encodes as ITSELF in 8 bits. The 240 position lengths
    and the 249 secondary lengths stay zero (no matches, and the decoder builds
    match tables lazily/allows the empty length tree), so a chunk costs ~0.5% of
    expansion over stored.

    Bit-exact against libmspack lzxd.c (the decoder XenonRecomp links and
    tools/big_decompress verifies with): bits pack MSB-first into 16-bit
    LITTLE-endian words; each chunk is an independent stream opening with 1 bit
    intel-E8 (0), 3 bits block type (1 = verbatim), 24 bits block length; then
    three pretree-coded length runs (20 x 4-bit pretree each) and the literal
    bits. Every emitted stream round-trips through tools/big_decompress before
    anything is written — the decoder is the oracle precisely because we did not
    write it."""
    out = bytearray()
    out += struct.pack('>II', len(data), 0x8000)
    for i in range(0, len(data), 0x8000):
        chunk = data[i:i + 0x8000]
        bits = []

        def put(val, n):
            for b in range(n - 1, -1, -1):
                bits.append((val >> b) & 1)

        def pretree(lengths):
            # 20 pretree code lengths, 4 bits each.
            for s in range(20):
                put(lengths.get(s, 0), 4)

        put(0, 1)              # no intel E8 translation
        put(1, 3)              # LZX_BLOCKTYPE_VERBATIM
        put(len(chunk), 24)

        # Main tree part 1: 256 literal lengths, all 8. Delta coding from the
        # zero-initialised state: symbol z with (0 - z) mod 17 == 8 is z = 9.
        # Pretree {9: len 1, 17: len 1} is complete; canonical gives 9 -> code 0.
        pretree({9: 1, 17: 1})
        for _ in range(256):
            put(0, 1)          # symbol 9

        # Main tree part 2: 240 position lengths, all zero, as 18-runs (20+val
        # zeros, 5 extra bits). Pretree {17: 1, 18: 1}: 17 -> 0, 18 -> 1.
        pretree({17: 1, 18: 1})
        for run in (51, 51, 51, 51, 36):
            put(1, 1)          # symbol 18
            put(run - 20, 5)

        # Length tree: 249 zeros, same idiom.
        pretree({17: 1, 18: 1})
        for run in (51, 51, 51, 51, 45):
            put(1, 1)
            put(run - 20, 5)

        # The content: symbol i has canonical code i at length 8.
        for b in chunk:
            put(b, 8)

        while len(bits) % 16:
            bits.append(0)
        payload = bytearray()
        for k in range(0, len(bits), 16):
            v = 0
            for b in bits[k:k + 16]:
                v = (v << 1) | b
            payload += struct.pack('<H', v)
        payload += b'\0\0\0\0'  # slack so the decoder's last ENSURE_BITS has bytes
        frame = bytes([0xFF]) + struct.pack('>HH', len(chunk), len(payload)) + \
            bytes(payload)
        out += struct.pack('>I', len(frame)) + frame
    return bytes(out)


# LZX position-code tables for the 32 KB window, from the tables in the IMAGE
# ITSELF (extra_bits at 0x820BC6F4, position_base at 0x820C8E38).
LZX_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
             9, 9, 10, 10, 11, 11, 12, 12, 13, 13]          # slots 0..29
LZX_BASE = []
_b = 0
for _e in LZX_EXTRA:
    LZX_BASE.append(_b)
    _b += 1 << _e


def huffman_lengths(freqs, maxlen):
    """Length-limited canonical Huffman code lengths (package-merge). freqs is a
    list; returns a list of code lengths (0 for unused symbols). Correct-by-
    construction Kraft equality when 2+ symbols are used; a single used symbol
    gets length 1 (a legal, decoder-accepted degenerate tree only reached by the
    LENGTH tree, and only when a block has exactly one distinct match length)."""
    used = [s for s, f in enumerate(freqs) if f]
    if not used:
        return [0] * len(freqs)
    if len(used) == 1:
        out = [0] * len(freqs)
        out[used[0]] = 1
        return out
    # package-merge
    packages = [[(freqs[s], (s,)) for s in used]]
    for _ in range(maxlen - 1):
        prev = sorted(packages[-1])
        merged = [(prev[i][0] + prev[i + 1][0], prev[i][1] + prev[i + 1][1])
                  for i in range(0, len(prev) - 1, 2)]
        packages.append(sorted(merged + [(freqs[s], (s,)) for s in used]))
    out = [0] * len(freqs)
    take = 2 * len(used) - 2
    for f, syms in sorted(packages[-1])[:take]:
        for s in syms:
            out[s] += 1
    assert sum(2 ** -l for l in out if l) == 1.0, 'package-merge broke Kraft'
    return out


def canonical_codes(lengths):
    """Canonical code assignment, identical to libmspack's table builder: sort by
    (length, symbol), codes count upward with left-justification per length."""
    syms = sorted((l, s) for s, l in enumerate(lengths) if l)
    codes = {}
    code = 0
    prev_len = 0
    for l, s in syms:
        code <<= (l - prev_len)
        codes[s] = (code, l)
        code += 1
        prev_len = l
    return codes


def lzx_encode_stream(data):
    """A real, small LZX compressor: greedy LZ77 over the full 32 KB window with
    per-chunk canonical Huffman trees, emitting VERBATIM blocks — the same shape
    the shipped encoder uses for the frontend text entries.

    WHY IT HAD TO BE REAL — the part-60 ladder, condensed. The guest crashes with
    heap corruption on every DEGENERATE stream this file's earlier emitters
    produced (stored entries, literal-only fixed-tree blocks of all three types),
    even when the plaintext was byte-identical to shipped content — while
    libmspack decodes every one of those streams perfectly. Two explanations
    survived that ladder (a decoder-table quirk the degenerate trees trip, or an
    in-place decompression margin the literal-heavy ratios violate) and BOTH are
    cured by the same thing: streams statistically like the shipped ones. Layout
    text compresses ~7-10x here, far inside every margin.

    Bit-exactness is checked two ways: every stream round-trips through
    tools/big_decompress (libmspack — the decoder lineage the recompiler links),
    and the structural conventions (canonical code assignment, delta/run tree
    encoding, R0-R2 offset history) are transcribed from lzxd.c, not guessed."""
    assert len(data) <= 0x8000, \
        f'lzx_encode_stream: {len(data)} bytes needs multiple chunks, and multi-' \
        'chunk streams from this encoder are REFUSED by the guest decoder'
    out = bytearray()
    out += struct.pack('>II', len(data), 0x8000)
    for start in range(0, len(data), 0x8000):
        chunk = data[start:start + 0x8000]
        n = len(chunk)

        # ---- pass 1: greedy LZ77 with an R0/R1/R2-aware cost preference
        ops = []                      # ('lit', byte) or ('m', offset, length)
        last = {}                     # 3-gram -> list of positions (most recent last)
        i = 0
        while i < n:
            best = None               # (length, offset)
            if i + 3 <= n:
                key = chunk[i:i + 3]
                for j in reversed(last.get(key, [])[-64:]):   # bounded chain
                    off = i - j
                    if off > 0x8000 - 2:
                        break
                    length = 3
                    limit = min(257, n - i)
                    while length < limit and chunk[j + length] == chunk[i + length]:
                        length += 1
                    if not best or length > best[0]:
                        best = (length, off)
                        if length >= 128:
                            break
            if best and best[0] >= 3:
                length, off = best
                ops.append(('m', off, length))
                for p in range(i, min(i + length, n - 2)):
                    last.setdefault(chunk[p:p + 3], []).append(p)
                i += length
            else:
                ops.append(('lit', chunk[i]))
                if i + 3 <= n:
                    last.setdefault(chunk[i:i + 3], []).append(i)
                i += 1

        # ---- pass 2: symbol frequencies (with the R-history resolved now, so
        # the emitted symbols are exactly the counted ones)
        def resolve(ops):
            R = [1, 1, 1]
            resolved = []
            for op in ops:
                if op[0] == 'lit':
                    resolved.append(op)
                    continue
                _, off, length = op
                if off == R[0]:
                    fmt = 0
                elif off == R[1]:
                    fmt = 1
                    R[1] = R[0]; R[0] = off
                elif off == R[2]:
                    fmt = 2
                    R[2] = R[0]; R[0] = off
                else:
                    fmt = off + 2
                    R[2] = R[1]; R[1] = R[0]; R[0] = off
                resolved.append(('m', fmt, length))
            return resolved

        rops = resolve(ops)
        main_freq = [0] * 496         # 256 literals + 30 slots * 8
        len_freq = [0] * 249
        for op in rops:
            if op[0] == 'lit':
                main_freq[op[1]] += 1
            else:
                _, fmt, length = op
                slot = max(s for s in range(30) if LZX_BASE[s] <= fmt)
                header = min(length - 2, 7)
                main_freq[256 + slot * 8 + header] += 1
                if header == 7:
                    len_freq[length - 2 - 7] += 1

        main_len = huffman_lengths(main_freq, 16)
        length_len = huffman_lengths(len_freq, 16)
        if not any(length_len):
            # no long matches this chunk: the tree would be EMPTY, and no shipped
            # stream has an empty LENGTH tree, so write a harmless two-code one
            length_len[0] = length_len[1] = 1
        main_codes = canonical_codes(main_len)
        length_codes = canonical_codes(length_len)

        bits = []

        def put(v, k):
            for b in range(k - 1, -1, -1):
                bits.append((v >> b) & 1)

        # ---- tree-length writer: the delta/17/18/19 run format, with a real
        # pretree per READ_LENGTHS call, exactly as the decoder consumes it
        def write_lengths(lens, prev):
            ops2 = []                 # (pretree symbol, extra-bits tuple)
            x = 0
            while x < len(lens):
                if lens[x] == 0:
                    run = 0
                    while x + run < len(lens) and lens[x + run] == 0:
                        run += 1
                    while run >= 20:
                        take = min(run, 51)
                        ops2.append((18, (take - 20, 5)))
                        run -= take; x += take
                    while run >= 4:
                        take = min(run, 19)
                        ops2.append((17, (take - 4, 4)))
                        run -= take; x += take
                    for _ in range(run):
                        ops2.append((((prev[x] - 0) % 17), None))
                        x += 1
                else:
                    ops2.append(((prev[x] - lens[x]) % 17, None))
                    x += 1
            pf = [0] * 20
            for s, _ in ops2:
                pf[s] += 1
            plens = huffman_lengths(pf, 15)
            if sum(1 for l in plens if l) == 1:
                # a one-code pretree: give it a legal complete partner
                lone = plens.index(1)
                plens[lone] = 1
                plens[(lone + 1) % 20] = 1
            pcodes = canonical_codes(plens)
            for s in range(20):
                put(plens[s], 4)
            for s, extra in ops2:
                c, l = pcodes[s]
                put(c, l)
                if extra:
                    put(extra[0], extra[1])

        put(0, 1)                     # no intel E8 translation
        put(1, 3)                     # LZX_BLOCKTYPE_VERBATIM
        put(n, 24)
        write_lengths(main_len[:256], [0] * 256)
        write_lengths(main_len[256:], [0] * 240)
        write_lengths(length_len, [0] * 249)

        # ---- content
        for op in rops:
            if op[0] == 'lit':
                c, l = main_codes[op[1]]
                put(c, l)
            else:
                _, fmt, length = op
                slot = max(s for s in range(30) if LZX_BASE[s] <= fmt)
                header = min(length - 2, 7)
                c, l = main_codes[256 + slot * 8 + header]
                put(c, l)
                if header == 7:
                    c, l = length_codes[length - 2 - 7]
                    put(c, l)
                eb = LZX_EXTRA[slot]
                if eb:
                    put(fmt - LZX_BASE[slot], eb)

        while len(bits) % 16:
            bits.append(0)
        payload = bytearray()
        for k in range(0, len(bits), 16):
            v = 0
            for b in bits[k:k + 16]:
                v = (v << 1) | b
            payload += struct.pack('<H', v)
        assert len(payload) < n, \
            f'lzx_encode_stream did not compress ({len(payload)} >= {n}) — this ' \
            'encoder is for layout TEXT; do not point it at compressed data'
        # FIVE ZERO TRAILER BYTES inside the chunk length, after the payload —
        # every shipped chunk has exactly this (measured across the frontend
        # entries), and it is decoder READAHEAD SLACK: a bit reader that pulls
        # 16-bit words ahead of consumption walks past cmpLen at stream end, and
        # without the slack it reads whatever follows — which is the corruption
        # this project spent a dozen probe runs chasing. libmspack buffers its
        # input and never showed it; the guest's decoder does not.
        frame = bytes([0xFF]) + struct.pack('>HH', n, len(payload)) + \
            bytes(payload) + b'\0' * 5
        out += struct.pack('>I', len(frame)) + frame
    return bytes(out)


def verify_fake_lzx(stored, want):
    """Round-trip a fake-compressed stream through tools/big_decompress and demand
    byte identity. The decoder is the oracle precisely because we did not write it."""
    import subprocess
    import tempfile
    exe = os.path.join(REPO, 'tools/big_decompress')
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'in')
        dst = os.path.join(td, 'out')
        open(src, 'wb').write(stored)
        r = subprocess.run([exe, src, dst], capture_output=True)
        assert r.returncode == 0, \
            f'fake LZX stream REFUSED by the decoder:\n{r.stdout.decode()}{r.stderr.decode()}'
        got = open(dst, 'rb').read()
        assert got == want, 'fake LZX stream decoded to DIFFERENT bytes'


def decompress_entry(stored):
    """A compressed entry is [LE u32 unc_size][LE u32 window][chunks of
    BE u16 unc / BE u16 comp, LZX]. Delegated to tools/big_decompress, which
    carries the LZX implementation (linked from XenonRecomp) and its own oracle."""
    import subprocess
    import tempfile
    exe = os.path.join(REPO, 'tools/big_decompress')
    assert os.path.exists(exe), 'build tools/big_decompress first (build_big_decompress.sh)'
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'in')
        dst = os.path.join(td, 'out')
        open(src, 'wb').write(stored)
        subprocess.run([exe, src, dst], check=True, capture_output=True)
        return open(dst, 'rb').read()


# ---------------------------------------------------------------------------
# The layout rewrite
# ---------------------------------------------------------------------------

# Row order, labels, and the baked value lists. Verbs use the 360 token order
# (ACT:<direction>:<name>) that every working screen on this build uses.
ROWS = [
    # (group name, label Text=, [(string id, comment), ...])
    ('Resolution',  '10718 IDS_OPTIONS_PC_RESOLUTION', [
        ('100000', 'CZ 1280x720'), ('100001', 'CZ 2560x1440'),
        ('100002', 'CZ 3840x2160'), ('100003', 'CZ 5120x2880')]),
    ('DisplayMode', '10719 IDS_OPTIONS_PC_DISPLAYMODE', [
        ('10724', 'IDS_OPTIONS_PC_WINDOWED'), ('100004', 'CZ Borderless'),
        ('10723', 'IDS_OPTIONS_PC_FULLSCREEN')]),
    ('VSync',       '10720 IDS_OPTIONS_PC_VSYNC', [
        ('10701', 'IDS_OPTIONS_OFF'), ('10700', 'IDS_OPTIONS_ON')]),
    ('Shadow',      '10752 IDS_OPTIONS_PC_SHADOW_QUALITY', [
        ('10754', 'IDS_OPTIONS_PC_LOW'), ('10755', 'IDS_OPTIONS_PC_MEDIUM'),
        ('10756', 'IDS_OPTIONS_PC_HIGH')]),
]

# The strings the shipped banks lack, added to every language identically (the
# resolution names are language-neutral; "Borderless" ships English everywhere,
# an accepted v1 shortcut recorded in the part-60 notes). Ids 100000+: the shipped bank's own maximum is 99999 (measured, not assumed - 60003 was taken).
NEW_STRINGS = {
    100000: '1280 x 720',
    100001: '2560 x 1440',
    100002: '3840 x 2160',
    100003: '5120 x 2880',
    100004: 'Borderless',
}

Y_FIRST, Y_STEP = 0.28473, 0.04861   # the shipped layout's own row rhythm


def parse_blocks(text):
    """The layout grammar is `TypeName Name\n{\n ... \n}` nested. Returns the
    line-index spans of every top-level-within-parent block, recursively usable:
    here we only need to find cFESpinGroup spans and CTSelect spans inside them."""
    lines = text.split('\n')
    return lines


def find_block(lines, start_pred, from_line=0):
    """Find (header_line, open_line, close_line) of the first block whose header
    matches start_pred at or after from_line. Brace-counting, exact."""
    for i in range(from_line, len(lines)):
        if start_pred(lines[i]):
            assert lines[i + 1].strip() == '{', f'no brace after line {i}: {lines[i]}'
            depth = 0
            for j in range(i + 1, len(lines)):
                s = lines[j].strip()
                if s == '{':
                    depth += 1
                elif s == '}':
                    depth -= 1
                    if depth == 0:
                        return i, i + 1, j
            raise AssertionError(f'unclosed block at line {i}')
    return None


def rewrite_options_pc(text):
    lines = text.split('\n')

    # 1. Collect every cFESpinGroup block span, by name.
    groups = {}
    pos = 0
    while True:
        hit = find_block(lines, lambda l: l.startswith('cFESpinGroup '), pos)
        if not hit:
            break
        h, o, c = hit
        groups[lines[h].split()[1]] = (h, o, c)
        pos = c + 1

    keep = [r[0] for r in ROWS]
    for g in groups:
        pass  # names known: Resolution DisplayMode VSync Multisampling Controller
              # Mouse Blur Zombie Shadow Texture
    missing = [k for k in keep if k not in groups]
    assert not missing, f'expected spin groups not found: {missing}'

    # 2. Rebuild: walk the file; HIDE unimplemented groups; transform kept ones.
    #    Kept groups are transformed as text spans so everything not named here
    #    stays byte-identical.
    #
    #    HIDDEN, NOT REMOVED. The first build deleted the six unimplemented spin
    #    groups outright and the screen crashed on OPEN, not on parse — the
    #    OptionsPC class's own setup code survives in the 360 XEX (unlike its verb
    #    handlers) and looks its known rows up by name, null-dereferencing any it
    #    cannot find. Visible="false" is the shipped layouts' own idiom for
    #    exactly this (options.txt hides its Import row that way): the widget
    #    exists for the code, the player never sees it, and the honest-menu rule
    #    is satisfied because a hidden row is not a dead row.
    out = []
    i = 0
    keep_index = {name: n for n, (name, _, _) in enumerate(ROWS)}
    while i < len(lines):
        hid = next((name for name, (h, o, c) in groups.items()
                    if h == i and name not in keep), None)
        if hid:
            # FULL body, hidden — minus its cFEAnim blocks. Three shapes were
            # tried and two REFUTED by crashes: deleting the group outright
            # null-derefs in the class's widget lookups at screen open, and a
            # minimal stub (group + bare CTSelect) null-calls in the engine's
            # deferred focus walk on an async worker. The class gets the whole
            # structure it expects; the anims go only to keep the layout inside
            # lzx_encode_stream's single-chunk limit, and an invisible row's
            # animations have nothing to show anyway.
            h, o, c = groups[hid]
            body = lines[h:c + 1]
            body.insert(2, 'Visible="false"')
            pruned = []
            j = 0
            while j < len(body):
                if body[j].startswith('cFEAnim '):
                    hit = find_block(body, lambda l: l is body[j], j)
                    # brace-count this block manually (find_block matches by
                    # identity above to reuse its scan)
                    depth = 0
                    k = j + 1
                    assert body[k].strip() == '{'
                    while k < len(body):
                        s = body[k].strip()
                        if s == '{':
                            depth += 1
                        elif s == '}':
                            depth -= 1
                            if depth == 0:
                                break
                        k += 1
                    j = k + 1
                    continue
                pruned.append(body[j])
                j += 1
            out.extend(pruned)
            i = c + 1
            continue
        gk = next((name for name, (h, o, c) in groups.items()
                   if h == i and name in keep), None)
        if gk:
            h, o, c = groups[gk]
            out.extend(transform_group(lines[h:c + 1], gk, keep_index[gk]))
            i = c + 1
            continue
        out.append(lines[i])
        i += 1
    # Un-hide the screen's art (the yellow-paper backdrop): the layout ships
    # mainmenu_art Visible="false" and the PC class's compiled-out enter was what
    # re-showed it — the operator's screenshot of the bare screen is the measurement.
    for k, l in enumerate(out):
        if l.startswith('cFEWidget mainmenu_art'):
            for j in range(k + 1, min(k + 5, len(out))):
                if out[j] == 'Visible="false"':
                    del out[j]
                    break
            break
    return '\n'.join(out)


def transform_group(glines, name, row_index):
    """Rewrite one kept spin group: focus chain, verbs, Y position, and the
    CTSelect cFEText -> cFETextList with our baked values."""
    order = [r[0] for r in ROWS]
    up = order[(row_index - 1) % len(order)]
    down = order[(row_index + 1) % len(order)]
    y = Y_FIRST + Y_STEP * row_index
    values = ROWS[row_index][2]

    # Header properties (they sit before the first nested cFE block).
    outl = []
    i = 0
    first_child = next(k for k, l in enumerate(glines)
                       if k >= 2 and l.startswith('cFE'))
    for k in range(first_child):
        l = glines[k]
        if l.startswith('onUp='):
            l = f'onUp="FOC:{up}"'
            if row_index == 0:
                # Initial focus. The working screens' CLASS enters focus their
                # first row from code; OptionsPC's enter is a compiled-out
                # default, so the layout declares it instead ('Focus' is in the
                # parser's own attribute table at 0x820B9398).
                outl.append('Focus="true"')
        elif l.startswith('onDown='):
            l = f'onDown="FOC:{down}"'
        elif l.startswith('onLeft='):
            l = f'onLeft="ACT:Prev:{name}"'
        elif l.startswith('onRight='):
            l = f'onRight="ACT:Next:{name}"'
        elif l.startswith('Y='):
            l = f'Y={y:.5f}'
        outl.append(l)
    body = glines[first_child:]

    # The two arrow buttons carry the same verbs on their onSelect.
    body = [re.sub(r'onSelect="ACT:[^"]*Prev[^"]*"', f'onSelect="ACT:Prev:{name}"', l)
            for l in body]
    body = [re.sub(r'onSelect="ACT:[^"]*Next[^"]*"', f'onSelect="ACT:Next:{name}"', l)
            for l in body]

    # Replace the CTSelect block (cFEText or cFETextList — the shipped PC layout
    # uses a bare cFEText that a PC-only runtime populated) with the WORKING
    # 360 idiom: a cFETextList with the values baked in.
    hit = find_block(body, lambda l: l.strip() in ('cFEText CTSelect',
                                                   'cFETextList CTSelect'))
    assert hit, f'{name}: no CTSelect block'
    h, o, c = hit
    text_lines = [f'Text="{sid} {comment}"' for sid, comment in values]
    new_block = ['cFETextList CTSelect', '{',
                 'Font="arialblk46"',
                 f'Size={len(values)}'] + text_lines + [
                 'Justify="center"',
                 'DropShadowX=0.00100',
                 'DropShadowY=0.00100',
                 'Loop="true"',
                 'Init=0',
                 'Y=0.04215',
                 'W=1.00000',
                 'H=1.00000',
                 'R=0.11765',
                 'G=0.39216',
                 'B=0.39216',
                 'ScaleX=0.55000',
                 'ScaleY=0.65000',
                 '}']
    body = body[:h] + new_block + body[c + 1:]
    return outl + body


# ---------------------------------------------------------------------------
# .bcs string banks
# ---------------------------------------------------------------------------

def patch_bcs(src, dst):
    d = open(src, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    ids = list(struct.unpack_from(f'<{n}I', d, 4))
    offs = list(struct.unpack_from(f'<{n}I', d, 4 + 4 * n))

    def string_at(off):
        end = d.index(b'\0', off)
        return d[off:end]

    table = {ids[k]: string_at(offs[k]) for k in range(n)}
    for i, s in NEW_STRINGS.items():
        assert i not in table, f'{src}: id {i} already exists — pick other ids'
        table[i] = s.encode('utf-8')

    new_ids = sorted(table)
    header = 4 + 4 * len(new_ids) * 2
    blob = bytearray()
    new_offs = []
    for i in new_ids:
        new_offs.append(header + len(blob))
        blob += table[i] + b'\0'
    out = struct.pack('<I', len(new_ids))
    out += struct.pack(f'<{len(new_ids)}I', *new_ids)
    out += struct.pack(f'<{len(new_ids)}I', *new_offs)
    out += bytes(blob)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(out)

    # Verify through the same reader shape the runtime implies: every original id
    # must yield its original bytes, every new id its new bytes.
    d2 = open(dst, 'rb').read()
    n2 = struct.unpack_from('<I', d2, 0)[0]
    ids2 = list(struct.unpack_from(f'<{n2}I', d2, 4))
    offs2 = list(struct.unpack_from(f'<{n2}I', d2, 4 + 4 * n2))
    got = {ids2[k]: d2[offs2[k]:d2.index(b'\0', offs2[k])] for k in range(n2)}
    assert got == table, f'{dst}: verification failed'
    return len(new_ids)


# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--print', action='store_true', dest='show',
                    help='print the rewritten options_pc.txt and exit')
    args = ap.parse_args()

    src_big = os.path.join(FRONTEND, 'fecmn.big')
    raw, data_start, names_off, entries = read_big(src_big)
    entry = next(e for e in entries if e['name'] == 'options_pc.txt')

    original = decompress_entry(entry['stored'])
    rewritten = rewrite_options_pc(original.decode('ascii'))
    if args.show:
        print(rewritten)
        return

    data = rewritten.encode('ascii')
    # COMPRESSED with the real encoder. Stored was measured to crash (the reader
    # decompresses in place, so the stream must be smaller than its plaintext),
    # and the literal-only fake_lzx_stream expands — its retained body above is
    # the map of that dead end.
    entry['stored'] = lzx_encode_stream(data)
    verify_fake_lzx(entry['stored'], data)
    entry['size2'] = len(data)

    dst_big = os.path.join(OUT_FRONTEND, 'fecmn.big')
    write_big(dst_big, raw, data_start, names_off, entries)

    # Verify the repack: parse it back; every entry byte-identical to what we
    # intended, including the ones we did not touch.
    raw2, ds2, no2, entries2 = read_big(dst_big)
    assert len(entries2) == len(entries)
    by_name = {e['name']: e for e in entries2}
    _, _, _, orig_entries = read_big(src_big)
    for e in orig_entries:
        want = entry['stored'] if e['name'] == 'options_pc.txt' else e['stored']
        got = by_name[e['name']]['stored']
        assert got == want, f'repack verification failed on {e["name"]}'
    print(f'{dst_big}: {len(entries2)} entries, options_pc.txt rewritten '
          f'({len(original)} -> {len(data)} bytes text, {len(entry["stored"])} compressed)')

    # THE COPY THE FRONTEND ACTUALLY READS is not the loose file: the boot preload
    # opens game:\data\preload4.big, which NESTS a compressed fecmn.big — the first
    # headless probe of part 60 opened every menu without a single fecmn.big
    # NtCreateFile line, which is how this was found.
    #
    # REPLACING the nested stream is a closed road, walked to the end before this
    # was written: the guest's preload decoder rejected every re-encode this script
    # can produce — stored bytes, LZX uncompressed-blocks, verbatim-literal blocks,
    # aligned-literal blocks, capped and uncapped chunk sizes — all of which
    # round-trip cleanly through libmspack (the decoder XenonRecomp itself links).
    # And a REAL re-compressor would not help either: the nested archive's content
    # is already-compressed data, the shipped margin over stored is under 1%, and
    # the chunk constraints leave no room for a literals-heavy encoding.
    #
    # So the nested copy is EVICTED instead: its index HASH is flipped so every
    # runtime lookup misses, while the stream bytes stay byte-identical (the boot
    # preload decodes it exactly as shipped — measured: the poisoned boot is clean).
    # On the miss, the engine falls back to opening data/frontend/fecmn.big LOOSE,
    # which the VFS overlay serves patched. The loose-archive path handles stored
    # entries as a matter of course (10,810 of the package's 12,481 entries are
    # stored), so the rewritten layout rides that proven road.
    src_pre = os.path.join(REPO, 'assets/game/data/preload4.big')
    raw_p, ds_p, no_p, entries_p = read_big(src_pre)
    fec = next(e for e in entries_p if e['name'] == 'fecmn.big')
    fec['hash'] ^= 0xDEADBEEF
    # Part 92: the nested fecmn.tex is evicted the same way, so the GLYPH bank is
    # also read through the loose file — where the keyboard-prompt overlay
    # (assets/game_kbm, tools/gen_kbm_icons.py) can serve the key-cap chips. With
    # that overlay absent or off the loose file is the shipped bytes, so this is
    # a null for a pad player.
    fect = next(e for e in entries_p if e['name'] == 'fecmn.tex')
    fect['hash'] ^= 0xDEADBEEF
    dst_pre = os.path.join(REPO, 'assets/game_patched/data/preload4.big')
    write_big(dst_pre, raw_p, ds_p, no_p, entries_p, align=0x800)

    raw_p2 = open(dst_pre, 'rb').read()
    src_raw = open(src_pre, 'rb').read()
    # The ONLY difference from the shipped archive must be the two hash words.
    diffs = [i for i in range(len(src_raw)) if src_raw[i] != raw_p2[i]] \
        if len(src_raw) == len(raw_p2) else None
    assert diffs is not None and len(diffs) <= 8, \
        f'preload4 poison changed more than the hashes: {None if diffs is None else len(diffs)} bytes'
    print(f'{dst_pre}: byte-identical to shipped except the nested fecmn.big and '
          f'fecmn.tex index hashes ({len(diffs)} bytes) — lookups miss, boot '
          f'decode unchanged')

    for f in sorted(os.listdir(FRONTEND)):
        if f.startswith('str_') and f.endswith('.bcs'):
            count = patch_bcs(os.path.join(FRONTEND, f),
                              os.path.join(OUT_FRONTEND, f))
            print(f'{OUT_FRONTEND}/{f}: {count} ids '
                  f'({len(NEW_STRINGS)} added, verified)')

    # layout.bin PINS EVERY FILE'S SIZE — 152-byte records of {128-byte path,
    # BE name-hash, BE size, 0, BE disc offset, 0, 0} — and the loader reads by
    # that size, not by asking the filesystem. The second part-60 probe proved it
    # the hard way: a preload4.big grown by 14 KB was read at its OLD size, the
    # nested fecmn truncated, and a loader thread died on a null callback. Every
    # file this overlay replaces gets its record's size updated; the disc offset
    # stays (files here are opened by NAME through the VFS, never by disc LBA).
    layout = bytearray(open(os.path.join(REPO, 'assets/game/layout.bin'), 'rb').read())
    overridden = ['data/preload4.big', 'data/frontend/fecmn.big'] + \
        [f'data/frontend/{f}' for f in sorted(os.listdir(OUT_FRONTEND))
         if f.startswith('str_')]
    patched_records = 0
    for i in range(len(layout) // 152):
        name = bytes(layout[i * 152:i * 152 + 128]).rstrip(b'\0').decode('ascii')
        if name in overridden:
            new_size = os.path.getsize(os.path.join(REPO, 'assets/game_patched', name))
            struct.pack_into('>I', layout, i * 152 + 132, new_size)
            patched_records += 1
    assert patched_records == len(overridden), \
        f'layout.bin: only {patched_records} of {len(overridden)} records found'
    dst_layout = os.path.join(REPO, 'assets/game_patched/layout.bin')
    open(dst_layout, 'wb').write(bytes(layout))
    print(f'{dst_layout}: {patched_records} size records updated')


if __name__ == '__main__':
    main()
