#!/usr/bin/env python3
"""Generate the patched frontend assets that wake the title's own PC graphics menu.

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


def write_big(path, raw_orig, data_start, names_off, entries):
    """Rebuild the archive: header + index + the ORIGINAL name table bytes +
    payloads packed in original file order at 4-byte alignment."""
    name_table = raw_orig[names_off:data_start]
    order = sorted(range(len(entries)), key=lambda i: entries[i]['data_off'])
    pos = data_start
    payload = bytearray()
    for i in order:
        e = entries[i]
        pad = (-pos) % 4
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

    # 2. Rebuild: walk the file; drop removed groups; transform kept ones.
    #    Kept groups are transformed as text spans so everything not named here
    #    stays byte-identical.
    drop_spans = [(o - 1, c) for name, (h, o, c) in groups.items() if name not in keep]

    out = []
    i = 0
    keep_index = {name: n for n, (name, _, _) in enumerate(ROWS)}
    while i < len(lines):
        span = next(((h, c) for name, (h, o, c) in groups.items()
                     if h == i and name not in keep), None)
        if span:
            i = span[1] + 1        # skip a removed group entirely
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
    entry['stored'] = data
    entry['size2'] = len(data)      # size == size2 -> the loader reads it STORED

    dst_big = os.path.join(OUT_FRONTEND, 'fecmn.big')
    write_big(dst_big, raw, data_start, names_off, entries)

    # Verify the repack: parse it back; every entry byte-identical to what we
    # intended, including the ones we did not touch.
    raw2, ds2, no2, entries2 = read_big(dst_big)
    assert len(entries2) == len(entries)
    by_name = {e['name']: e for e in entries2}
    _, _, _, orig_entries = read_big(src_big)
    for e in orig_entries:
        want = data if e['name'] == 'options_pc.txt' else e['stored']
        got = by_name[e['name']]['stored']
        assert got == want, f'repack verification failed on {e["name"]}'
    print(f'{dst_big}: {len(entries2)} entries, options_pc.txt rewritten '
          f'({len(original)} -> {len(data)} bytes, stored uncompressed)')

    for f in sorted(os.listdir(FRONTEND)):
        if f.startswith('str_') and f.endswith('.bcs'):
            count = patch_bcs(os.path.join(FRONTEND, f),
                              os.path.join(OUT_FRONTEND, f))
            print(f'{OUT_FRONTEND}/{f}: {count} ids '
                  f'({len(NEW_STRINGS)} added, verified)')


if __name__ == '__main__':
    main()
