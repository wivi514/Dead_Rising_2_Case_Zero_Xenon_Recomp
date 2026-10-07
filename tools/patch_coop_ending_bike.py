#!/usr/bin/env python3
"""Give the co-op partner his own motorcycle at the military arrival.

WHY THIS EXISTS
---------------
At the end of the game the host rides out on a motorcycle and the joining
Chuck stays on foot, beside him at the gas station (operator, 2026-10-07).
missions.txt (data/datafile.big; preload4.big carries a copy the game does not
read for this) states why, in
PrologueMilitaryArrival's `cMissionLevelReady PrologueCase1-Start3`:

    cMissionSpawnItem Bike1 { ItemName = "BrokenBike" ...
        cMissionSetChuckState OnBike1 { ChuckState = "17" Item = "Bike1" } }

  * ONE ridable bike. items.txt has a single cBikeItem, BrokenBike, with one
    seat; the only other BrokenBike spawn is a static prop in the safehouse.
  * The mount, ChuckState 17, has NO `Value`. Its Execute (0x82409900, state
    17 at 0x8240A3E4) queues the mount only where `Value == world->0x80`, the
    LOCAL player index, so only the host (index 0) mounts. The mount itself
    (0x823AB488) seats the LOCAL player — GetUserPlayer(world->0x80) — on the
    prop found by the action's `Item` name.

So this adds a second bike, CoopBike, beside Bike1 (the name Bike2 is TAKEN: a static BikeBody prop in the safehouse, and the mount finds its bike BY NAME), whose mount says `Value = 1`:
on the joiner (local index 1) it queues and seats the joiner's own Chuck.
`CoopOnly = "true"` keeps single player byte-for-byte as shipped: the spawn's
Execute (sub_823A5238) reads that byte at +0x63 and returns before spawning
when sub_82553270 (is-co-op) says no. Both machines run the patched data, so
both spawn CoopBike in the same order and item serials stay in step (the
co-op item-identity rule, coop-plan.md "Player issue #11").

Bike1 is at (-262.137, 3.896, -47.381) with a yaw of ~37.7 degrees
(quaternion y = 0.323). CoopBike sits 2.5 units along the bike's own +X — beside
it, not in front — at the same rotation.

Ported to runtime/host/overlay_gen.cpp (RewriteMissions) for the release; the
C++ is byte-identical to this (`cz_runtime --gen-overlays` + `diff -r` is the
gate). Change both, and bump kGeneratorVersion, in the same commit.
Run after tools/patch_coop_outfit.py: both edit the same two archives and each
reads the overlay's copy when it exists.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_pc_options import (REPO, read_big, write_big, decompress_entry,  # noqa: E402
                            lzx_encode_stream, verify_fake_lzx)

BIKE1 = (
    '\tcMissionSpawnItem Bike1\n'
    '\t\t{\n'
    '\t\t\tIgnoreAction = "false"\n'
    '\t\t\tItemName = "BrokenBike"\n'
    '\t\t\tLocation = "-262.137,3.896,-47.381"\n'
    '\t\t\tRotation = "0.015,0.323,-0.005,0.946"\n'
    '\t\t\tcMissionSetChuckState OnBike1\n'
    '\t\t\t{\n'
    '\t\t\t\tChuckState = "17"\n'
    '\t\t\t\tIgnoreAction = "false"\n'
    '\t\t\t\tItem = "Bike1"\n'
    '\t\t\t}\n'
    '\n'
    '\t\t}\n'
)
BIKE2 = (
    '\n'
    '\t\tcMissionSpawnItem CoopBike\n'
    '\t\t{\n'
    '\t\t\tCoopOnly = "true"\n'
    '\t\t\tIgnoreAction = "false"\n'
    '\t\t\tItemName = "BrokenBike"\n'
    '\t\t\tLocation = "-260.159,3.896,-48.910"\n'
    '\t\t\tRotation = "0.015,0.323,-0.005,0.946"\n'
    '\t\t\tcMissionSetChuckState OnCoopBike\n'
    '\t\t\t{\n'
    '\t\t\t\tChuckState = "17"\n'
    '\t\t\t\tIgnoreAction = "false"\n'
    '\t\t\t\tItem = "CoopBike"\n'
    '\t\t\t\tValue = "1"\n'
    '\t\t\t}\n'
    '\n'
    '\t\t}\n'
)


# The shipped missions.txt is CRLF throughout (10,877 CRs); the blocks above are
# written with \n for legibility and converted here.
BIKE1_CRLF = BIKE1.replace('\n', '\r\n')
BIKE2_CRLF = BIKE2.replace('\n', '\r\n')


def rewrite(text):
    """missions.txt with CoopBike after Bike1, or None if it is already there."""
    if 'cMissionSpawnItem CoopBike\r\n' in text:
        return None
    assert text.count(BIKE1_CRLF) == 1, 'the Bike1 block is not the shape this tool expects'
    return text.replace(BIKE1_CRLF, BIKE1_CRLF + BIKE2_CRLF)


def patch_archive(rel, align):
    src = os.path.join(REPO, 'assets/game_patched', rel)
    if not os.path.exists(src):
        src = os.path.join(REPO, 'assets/game', rel)
    dst = os.path.join(REPO, 'assets/game_patched', rel)
    raw, data_start, names_off, entries = read_big(src)
    entry = next(e for e in entries if e['name'] == 'missions.txt')
    text = decompress_entry(entry['stored']).decode('ascii')
    new_text = rewrite(text)
    if new_text is None:
        print(f'{src}: missions.txt already patched')
        return
    data = new_text.encode('ascii')
    # COMPRESSED, in seven chunks. Two shapes were refused first, by measurement:
    #   * STORED (size == size2, resv 0): the loader reads datafile.big's text
    #     entries with an in-place decompress and asserts
    #     "compressed block too large for in-place decompression or data is
    #     corrupt" (compression.cpp:676), then null-calls on an online thread.
    #   * this repo's encoder refused >32 KB on the strength of preload4.big's
    #     boot preload, which rejects every re-encode. The loose datafile.big path
    #     decodes the 7-chunk stream: no fault, no assert, the CoopBike names in
    #     guest memory.
    # So datafile.big ONLY; preload4.big's copy stays as shipped (the game reads
    # the datafile.big copy).
    entry['stored'] = lzx_encode_stream(data, multi_chunk=True)
    verify_fake_lzx(entry['stored'], data)
    entry['size2'] = len(data)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    write_big(dst, raw, data_start, names_off, entries, align=align)
    _, _, _, entries2 = read_big(dst)
    by_name = {e['name']: e for e in entries2}
    for e in entries:
        assert by_name[e['name']]['stored'] == e['stored'], f'{dst}: repack failed on {e["name"]}'
    assert decompress_entry(by_name['missions.txt']['stored']) == data
    print(f'{dst}: missions.txt rewritten ({len(text)} -> {len(data)} bytes text, '
          f'{len(entry["stored"])} compressed in 7 chunks); CoopBike (CoopOnly, Value 1) after Bike1')


def update_layout(rel):
    """layout.bin PINS EVERY FILE'S SIZE (152-byte records, BE size at +132) and the
    loader reads by that size (gen_pc_options.py, part 60). This archive grows by
    ~4 KB, so its record must follow it — overlay_gen.cpp already lists
    datafile.big among the files it re-sizes; this keeps the dev overlay equal."""
    path = os.path.join(REPO, 'assets/game_patched/layout.bin')
    if not os.path.exists(path):
        path_src = os.path.join(REPO, 'assets/game/layout.bin')
    else:
        path_src = path
    layout = bytearray(open(path_src, 'rb').read())
    size = os.path.getsize(os.path.join(REPO, 'assets/game_patched', rel))
    found = 0
    for i in range(0, len(layout) - 151, 152):
        if bytes(layout[i:i + 128]).rstrip(b'\0').decode('ascii') == rel:
            struct.pack_into('>I', layout, i + 132, size)
            found += 1
    assert found == 1, f'layout.bin: {found} records for {rel}'
    open(path, 'wb').write(bytes(layout))
    print(f'{path}: {rel} pinned at {size} bytes')


def main():
    patch_archive('data/datafile.big', align=0x800)
    update_layout('data/datafile.big')


if __name__ == '__main__':
    main()
