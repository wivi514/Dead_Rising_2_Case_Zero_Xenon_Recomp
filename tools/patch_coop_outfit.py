#!/usr/bin/env python3
"""Give the co-op partner an outfit Case Zero actually ships.

WHY THIS EXISTS
---------------
The first two-machine co-op session (co-op plan part 3, 2026-09-11) put the
joining Chuck in Still Creek with legs, hands, boots and no torso. The
operator's guess was right the moment they changed clothes and the torso
appeared: the client spawns in the co-op default outfit, and that row of
data/datafile.big -> outfits.csv is Dead Rising 2's, not Case Zero's:

    OUTFIT_COOP_DEFAULT,base,naked,NONE,champions_jacket2,Banana_Hammock,default,Default_Riding_Boots_under,

`chest_champions_jacket2` (DR2's partner jacket) is in no archive of this
package (`big_list.py --all --find champions` -> 0); every other piece of the
row is. So the chest piece becomes `default` — chest_default.big/.tex ARE
shipped, DR2 Chuck's own leather jacket — which keeps the partner visibly
different from the host's young_chuck outfit and complete.

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

OLD_ROW = 'OUTFIT_COOP_DEFAULT,base,naked,NONE,champions_jacket2,Banana_Hammock,default,Default_Riding_Boots_under,'
NEW_ROW = 'OUTFIT_COOP_DEFAULT,base,naked,NONE,default,Banana_Hammock,default,Default_Riding_Boots_under,'


def patch_archive(rel, align):
    src = os.path.join(REPO, 'assets/game_patched', rel)
    if not os.path.exists(src):
        src = os.path.join(REPO, 'assets/game', rel)
    dst = os.path.join(REPO, 'assets/game_patched', rel)
    raw, data_start, names_off, entries = read_big(src)
    entry = next(e for e in entries if e['name'] == 'outfits.csv')
    text = decompress_entry(entry['stored']).decode('ascii')
    if NEW_ROW in text and OLD_ROW not in text:
        print(f'{src}: outfits.csv already patched')
        return
    assert OLD_ROW in text, f'{src}: the DR2 co-op default row is not what this tool expects'
    new_text = text.replace(OLD_ROW, NEW_ROW)
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
          f'{len(entry["stored"])} compressed); OUTFIT_COOP_DEFAULT chest champions_jacket2 -> default')


def main():
    patch_archive('data/datafile.big', align=0x800)   # the shipped layout is 0x800-aligned
    patch_archive('data/preload4.big', align=0x800)


if __name__ == '__main__':
    main()
