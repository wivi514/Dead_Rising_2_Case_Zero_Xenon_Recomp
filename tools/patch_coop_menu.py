#!/usr/bin/env python3
"""Put "JOIN CO-OP GAME" on Case Zero's main menu, through the title's own screens.

WHY THIS EXISTS
---------------
Co-op plan parts 2-4 (docs/coop-plan.md) made this build host and join co-op
sessions, but the joiner's side was an environment variable: CZ_XLIVE_JOIN=1
opens the title's GameSelect screen in join mode from C++ (kernel/coop_join.cpp)
because Case Zero's menus never do. The operator's instruction for part 5 was
to start co-op from the MAIN MENU, like a player would.

The menus are DATA, and the join screens SHIP. data/frontend/mainmenu.big is a
.big of cFEScreen layout scripts (docs/big-archive-format.md; the grammar is
gen_pc_options.py's, part 60): title.txt is the main menu, and joingame.txt is
DR2's JoinGame screen — its "JOIN XBOX LIVE GAME" row raises ACT:XboxLive,
whose handler (sub_824DAA10) opens GameSelect in mode 1, the exact call
coop_join.cpp makes. The screen class, the row handler and the whole join
flow below it are linked; only the ROW that leads to the screen was cut from
title.txt. So this tool puts a row back, and nothing in C++ changes:

  title.txt      a cFEButton JoinCoop cloned from StartPrologue — same
                 highlight, grunge and text animations, so it looks like the
                 rows around it — with Text = string 108 "JOIN CO-OP GAME"
                 (shipped in every language bank) and onSelect="FWD:JoinGame",
                 the framework's own forward-transition verb. It sits under
                 START GAME; every row below moves down one slot (the layout's
                 own 0.05556 rhythm) and the onUp/onDown focus chain is
                 re-linked around it. The TitleScreen class hides rows by
                 NAME (sub_824D8A00 hides TrialUnlock and moves ExitToArcade
                 up when the game is licensed; sub_824D88C0 hides
                 DebugJumpPrologue), so a new name is invisible to it.
  joingame.txt   the "JOIN FRIENDS" row is REMOVED. It raises ACT:Friends,
                 which posts a friends-list request the kernel answers with an
                 honest failure — a row that does nothing is the gamma slider
                 all over again (part60-kickoff §3). The remaining row's focus
                 chain points at itself.
  path_fe.txt    (in fecmn.big) TitleScreen gains a JoinGame=">Normal" edge.
                 The manifest is the frontend's transition graph — which
                 screens a screen may open and which file backs each — and
                 the shipped one lists JoinGame only under GameSelect.

All three land in the part-60 patched-asset overlay (assets/game_patched/,
served by kernel/vfs.cpp over the package). fecmn.big is rewritten in BOTH
overlays that carry it — game_patched and game_bootskip (part 99's
intro-collapsed copy, served instead of game_patched's while the launcher's
skip-intro toggle is on) — so the row works with the toggle either way. Every
rewritten entry is re-encoded with gen_pc_options.py's LZX encoder, and every
changed file's size is written into the overlay's layout.bin, which pins file
sizes (the loader reads by that record, not by asking the filesystem —
measured in part 60 with a truncated nested archive).

Run AFTER tools/gen_pc_options.py (which writes the overlay's fecmn.big,
bootskip copy and layout.bin this reads) and beside tools/patch_coop_outfit.py.
Idempotent: a second run finds the rows in place and says so. Not yet ported to
runtime/host/overlay_gen.cpp — a release with co-op needs that; a solo release
does not.

The player's route afterwards: main menu → JOIN CO-OP GAME → JOIN XBOX LIVE
GAME → the "unsaved progress" dialog → a save slot → the search, the QoS probe
and the join, all the title's own (coop-plan.md part 3). CZ_XLIVE_JOIN stays as
the headless lever; the two are the same call.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_pc_options import (REPO, read_big, write_big, decompress_entry,  # noqa: E402
                            lzx_encode_stream, verify_fake_lzx, find_block)

PATCHED = os.path.join(REPO, 'assets/game_patched')
BOOTSKIP = os.path.join(REPO, 'assets/game_bootskip')

NEW_ROW = 'JoinCoop'
NEW_TEXT = '108 IDS_JOIN_COOP_GAME'      # "JOIN CO-OP GAME", shipped in every bank
NEW_SELECT = 'FWD:JoinGame'
# The shipped rows' Y values, top to bottom, and the slot after the last: the
# layout's own rhythm, copied rather than computed so a diff against the
# shipped file reads as a one-slot shift and nothing else.
SLOT_Y = ['0.47222', '0.54167', '0.59722', '0.65278', '0.70833', '0.76389',
          '0.81944', '0.87500']


def block_field(lines, open_line, close_line, key):
    """Line index of `key=` at depth 1 of the block (its own field, not a child's)."""
    depth = 0
    for i in range(open_line, close_line + 1):
        s = lines[i].strip()
        if s == '{':
            depth += 1
        elif s == '}':
            depth -= 1
        elif depth == 1 and s.startswith(key + '='):
            return i
    raise AssertionError(f'no {key}= at depth 1 in block at line {open_line}')


def rewrite_title(text):
    """title.txt with the JoinCoop row, or None if it is already there."""
    if f'cFEButton {NEW_ROW}' in text:
        return None
    lines = text.split('\n')
    is_button = lambda name: (lambda l: l.strip() == f'cFEButton {name}')
    h, o, c = find_block(lines, is_button('StartPrologue'))
    start = lines[h:c + 1]
    assert start[block_field(start, 1, len(start) - 1, 'onDown')].strip() == 'onDown="FOC:Leaderboard"'
    assert start[block_field(start, 1, len(start) - 1, 'Y')].strip() == f'Y={SLOT_Y[1]}'

    # The clone: same widget tree, new name, verbs, slot and label.
    row = list(start)
    row[0] = f'cFEButton {NEW_ROW}'
    row[block_field(row, 1, len(row) - 1, 'onUp')] = 'onUp="FOC:StartPrologue"'
    row[block_field(row, 1, len(row) - 1, 'onDown')] = 'onDown="FOC:Leaderboard"'
    row[block_field(row, 1, len(row) - 1, 'onSelect')] = f'onSelect="{NEW_SELECT}"'
    row[block_field(row, 1, len(row) - 1, 'Y')] = f'Y={SLOT_Y[2]}'
    labels = [i for i, l in enumerate(row) if l.startswith('Text=')]
    assert labels == [i for i in labels if row[i] == 'Text="107 IDS_START_GAME"'] and len(labels) == 1
    row[labels[0]] = f'Text="{NEW_TEXT}"'

    # Re-link the chain around it and shift every row below down one slot.
    lines[h + block_field(start, 1, len(start) - 1, 'onDown')] = f'onDown="FOC:{NEW_ROW}"'
    lines[c + 1:c + 1] = row
    shifted = 0
    for name, slot in (('Leaderboard', 3), ('Achievements', 4), ('OptionsPrologue', 5),
                       ('TrialUnlock', 6), ('ExitToArcade', 7)):
        h2, o2, c2 = find_block(lines, is_button(name))
        yi = block_field(lines, o2, c2, 'Y')
        assert lines[yi] == f'Y={SLOT_Y[slot - 1]}', f'{name}: {lines[yi]} is not slot {slot - 1}'
        lines[yi] = f'Y={SLOT_Y[slot]}'
        shifted += 1
    h3, o3, c3 = find_block(lines, is_button('Leaderboard'))
    ui = block_field(lines, o3, c3, 'onUp')
    assert lines[ui] == 'onUp="FOC:StartPrologue"'
    lines[ui] = f'onUp="FOC:{NEW_ROW}"'
    assert shifted == 5
    return '\n'.join(lines)


def rewrite_joingame(text):
    """joingame.txt without the Friends row, or None if already so."""
    if 'cFEButton Friends' not in text:
        return None
    lines = text.split('\n')
    h, o, c = find_block(lines, lambda l: l.strip() == 'cFEButton Friends')
    del lines[h:c + 1]
    h2, o2, c2 = find_block(lines, lambda l: l.strip() == 'cFEButton XboxLive')
    for key in ('onUp', 'onDown'):
        i = block_field(lines, o2, c2, key)
        assert lines[i] == f'{key}="FOC:Friends"'
        lines[i] = f'{key}="FOC:XboxLive"'
    assert 'Friends' not in '\n'.join(lines)
    return '\n'.join(lines)


def rewrite_path_fe(text):
    """path_fe.txt with TitleScreen -> JoinGame, or None if already so."""
    lines = text.split('\n')
    h, o, c = find_block(lines, lambda l: l.strip() == 'TitleScreen')
    block = lines[o + 1:c]
    if 'JoinGame=">Normal"' in block:
        return None
    assert 'GameSelect=">Normal"' in block, 'TitleScreen manifest block is not the shipped shape'
    lines.insert(o + 1 + block.index('GameSelect=">Normal"') + 1, 'JoinGame=">Normal"')
    return '\n'.join(lines)


def patch_archive(src, dst, rewrites, align):
    """Apply {entry name: rewrite fn} to the archive at src, writing dst.
    Returns the names actually rewritten."""
    raw, data_start, names_off, entries = read_big(src)
    done = []
    for name, fn in rewrites.items():
        entry = next(e for e in entries if e['name'] == name)
        text = decompress_entry(entry['stored']).decode('ascii')
        new_text = fn(text)
        if new_text is None:
            continue
        data = new_text.encode('ascii')
        entry['stored'] = lzx_encode_stream(data)
        verify_fake_lzx(entry['stored'], data)
        entry['size2'] = len(data)
        done.append((name, len(text), len(data), len(entry['stored'])))
    if not done:
        return []
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    write_big(dst, raw, data_start, names_off, entries, align=align)
    # Read it back the way the runtime will: every entry as intended.
    _, _, _, entries2 = read_big(dst)
    by_name = {e['name']: e for e in entries2}
    for e in entries:
        assert by_name[e['name']]['stored'] == e['stored'], f'{dst}: repack failed on {e["name"]}'
    return done


def overlay_src(rel, layer=PATCHED):
    """The overlay's copy when it exists (an earlier tool's edits survive), else the package's."""
    p = os.path.join(layer, rel)
    return p if os.path.exists(p) else os.path.join(REPO, 'assets/game', rel)


def update_layout(changed):
    """Write the changed files' sizes into the overlay's layout.bin (which
    gen_pc_options.py creates; the shipped one is the fallback)."""
    src = overlay_src('layout.bin')
    layout = bytearray(open(src, 'rb').read())
    found = 0
    for i in range(len(layout) // 152):
        name = bytes(layout[i * 152:i * 152 + 128]).rstrip(b'\0').decode('ascii')
        if name in changed:
            struct.pack_into('>I', layout, i * 152 + 132,
                             os.path.getsize(os.path.join(PATCHED, name)))
            found += 1
    assert found == len(changed), f'layout.bin: {found} of {len(changed)} records found'
    dst = os.path.join(PATCHED, 'layout.bin')
    open(dst, 'wb').write(bytes(layout))
    print(f'{dst}: {found} size record(s) updated')


def main():
    changed = []
    rel = 'data/frontend/mainmenu.big'
    done = patch_archive(overlay_src(rel), os.path.join(PATCHED, rel),
                         {'title.txt': rewrite_title, 'joingame.txt': rewrite_joingame},
                         align=4)   # the shipped archive packs at 4 bytes
    for name, before, after, comp in done:
        print(f'{rel}: {name} rewritten ({before} -> {after} bytes text, {comp} compressed)')
    if done:
        changed.append(rel)
    else:
        print(f'{rel}: already patched')

    rel = 'data/frontend/fecmn.big'
    for layer in (PATCHED, BOOTSKIP):
        src = overlay_src(rel, layer)
        if layer is BOOTSKIP and not src.startswith(BOOTSKIP):
            print(f'{layer}/{rel}: absent (gen_pc_options.py writes it); skipped')
            continue
        done = patch_archive(src, os.path.join(layer, rel), {'path_fe.txt': rewrite_path_fe},
                             align=4)
        for name, before, after, comp in done:
            print(f'{layer}/{rel}: {name} rewritten ({before} -> {after} bytes text, '
                  f'{comp} compressed): TitleScreen -> JoinGame')
        if done and layer is PATCHED:
            changed.append(rel)
        elif not done:
            print(f'{layer}/{rel}: already patched')
    if changed:
        update_layout(changed)


if __name__ == '__main__':
    main()
