#!/usr/bin/env python3
"""Give the co-op partner the outfit player one spawns in.

WHY THIS EXISTS
---------------
The first two-machine co-op session (co-op plan part 3, 2026-09-11) put the
joining Chuck in Still Creek with legs, hands, boots and no torso — on both
machines. Part 4 instrumented the clothing pipeline (CZ_OUTFIT_TRACE=1,
runtime/kernel/coop_outfit.cpp) instead of guessing, and the trace named it:

  * The client reports its SAVE outfit (OUTFIT_DEFAULT_UNDER: young_chuck
    everywhere) and the host records it correctly for the remote player.
  * The load requester, sub_82270290, has a special case FOR THE CHEST ONLY:
    a player other than player 0 asking for OUTFIT_DEFAULT_UNDER's chest
    (entry 17, db+0x1364) is handed OUTFIT_COOP_DEFAULT_UNDER's chest instead
    (entry 16, db+0x1248), and player 0 the reverse — DR2's way of making the
    two Chucks look different (the partner wears the champion's jacket).
  * Case Zero's outfits.csv has NO OUTFIT_COOP_DEFAULT_UNDER row. The name is
    in the title's table (0x829D44F8, index 16) but the csv loader
    (sub_821B67C0) fills entries by name and this one is never filled, so it
    holds the constructor's seven "NONE"s — and the chest the title then asks
    for is `chest_NONE`, which nothing can load. Every other part is untouched,
    which is why only the torso was missing.

The OUTFIT_COOP_DEFAULT (_over) row exists but names chest_champions_jacket2,
DR2's partner jacket, which is in no archive of this package; that row is
what the first version of this tool patched (chest -> default) and it
measured a null, because the chest substitution reads the _UNDER row.

The operator's instruction (2026-09-11): the partner should wear the basic
outfit player one spawns in. So both co-op rows become copies of the default
rows — OUTFIT_COOP_DEFAULT = OUTFIT_DEFAULT and OUTFIT_COOP_DEFAULT_UNDER =
OUTFIT_DEFAULT_UNDER — and the chest substitution becomes the identity in
both directions. (To make the partner visibly different instead, change the
chest of both co-op rows to `default`: chest_default.big/.tex ARE shipped,
DR2 Chuck's leather jacket.)

The rewritten entry is written to the part-60 patched-asset overlay
(assets/game_patched/, served by kernel/vfs.cpp over the package) in BOTH
archives that carry outfits.csv, data/datafile.big and data/preload4.big,
because the boot opens both and which copy answers the hash lookup was not
established — patching one and evicting the other would have needed the
fallback path known, and this is the shape gen_pc_options.py proved for
fecmn.big. The stream is re-encoded with that script's LZX encoder (a
1.8 KB text compresses well; the preload decoder's objection there was to
re-encoding already-compressed data, not to LZX).

Run after tools/gen_pc_options.py, which writes game_patched/data/preload4.big
first; this reads the overlay's copy when it exists so both edits survive.
Not yet ported to runtime/host/overlay_gen.cpp: a release with co-op needs
that (release A.1's first-run overlay generation), and a solo release does
not, because a solo game never dresses a partner.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_pc_options import (REPO, read_big, write_big, decompress_entry,  # noqa: E402
                            lzx_encode_stream, verify_fake_lzx)

# The three shapes of the OUTFIT_COOP_DEFAULT row this tool may find: stock,
# the first version's patch, and the final form.
COOP_STOCK = 'OUTFIT_COOP_DEFAULT,base,naked,NONE,champions_jacket2,Banana_Hammock,default,Default_Riding_Boots_under,'
COOP_V1 = 'OUTFIT_COOP_DEFAULT,base,naked,NONE,default,Banana_Hammock,default,Default_Riding_Boots_under,'
DEFAULT_ROW = 'OUTFIT_DEFAULT,young_chuck,young_chuck,NONE,young_chuck,naked,young_chuck,young_chuck_over,'
DEFAULT_UNDER_ROW = 'OUTFIT_DEFAULT_UNDER,young_chuck,young_chuck,NONE,young_chuck,naked,young_chuck,young_chuck_under,'
COOP_ROW = DEFAULT_ROW.replace('OUTFIT_DEFAULT,', 'OUTFIT_COOP_DEFAULT,')
COOP_UNDER_ROW = DEFAULT_UNDER_ROW.replace('OUTFIT_DEFAULT_UNDER,', 'OUTFIT_COOP_DEFAULT_UNDER,')


def rewrite(text):
    """The csv with both co-op rows in their final form, or None if nothing to do."""
    if COOP_ROW in text and COOP_UNDER_ROW in text:
        return None
    for old in (COOP_STOCK, COOP_V1):
        text = text.replace(old, COOP_ROW)
    assert COOP_ROW in text, 'the OUTFIT_COOP_DEFAULT row is not any shape this tool expects'
    assert DEFAULT_UNDER_ROW in text, 'the OUTFIT_DEFAULT_UNDER row is not what this tool expects'
    if COOP_UNDER_ROW not in text:
        # The loader places a row by NAME (table index 16), so position is free;
        # next to the row it mirrors is where a reader would look for it.
        text = text.replace(DEFAULT_UNDER_ROW, DEFAULT_UNDER_ROW + '\n' + COOP_UNDER_ROW)
    return text


def patch_archive(rel, align):
    src = os.path.join(REPO, 'assets/game_patched', rel)
    if not os.path.exists(src):
        src = os.path.join(REPO, 'assets/game', rel)
    dst = os.path.join(REPO, 'assets/game_patched', rel)
    raw, data_start, names_off, entries = read_big(src)
    entry = next(e for e in entries if e['name'] == 'outfits.csv')
    text = decompress_entry(entry['stored']).decode('ascii')
    new_text = rewrite(text)
    if new_text is None:
        print(f'{src}: outfits.csv already patched')
        return
    data = new_text.encode('ascii')
    entry['stored'] = lzx_encode_stream(data)
    verify_fake_lzx(entry['stored'], data)
    entry['size2'] = len(data)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    write_big(dst, raw, data_start, names_off, entries, align=align)
    # Read it back the way the runtime will: every entry as intended.
    _, _, _, entries2 = read_big(dst)
    by_name = {e['name']: e for e in entries2}
    for e in entries:
        assert by_name[e['name']]['stored'] == e['stored'], f'{dst}: repack failed on {e["name"]}'
    assert decompress_entry(by_name['outfits.csv']['stored']) == data
    print(f'{dst}: outfits.csv rewritten ({len(text)} -> {len(data)} bytes text, '
          f'{len(entry["stored"])} compressed); OUTFIT_COOP_DEFAULT = OUTFIT_DEFAULT, '
          f'OUTFIT_COOP_DEFAULT_UNDER added = OUTFIT_DEFAULT_UNDER')


def main():
    patch_archive('data/datafile.big', align=0x800)   # the shipped layout is 0x800-aligned
    patch_archive('data/preload4.big', align=0x800)


if __name__ == '__main__':
    main()
