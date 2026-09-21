#!/usr/bin/env python3
"""The title's own string hash (`sub_8276E398`), in Python, plus a hash -> name table.

WHY THIS EXISTS
---------------
Items, props and mission events are carried around this engine as a 32-bit hash of
their NAME and never as the name itself: an item object's `+0x100` is what the guest
compares against `hash("WheelPawn")`, and a trace of the item a player is holding can
therefore only print a number. A number is not evidence anybody can read, so this
reverses it against the populations the game actually ships — `items.txt` (1,700-odd
item definitions), plus any extra names given on the command line.

The hash is transcribed from `sub_8276E398`, which is the classic `h = h*33 ^ c` with
a SIGNED char and 32-bit wrap:

    h = 0
    for c in name[:len]:        # stops early at a NUL
        h = ((h * 0x21) ^ sign_extend8(c)) & 0xFFFFFFFF

Usage:
    tools/name_hash.py WheelPawn BikeEngine        # name -> hash
    tools/name_hash.py --lookup 1A2B3C4D ...       # hash -> name, from items.txt
    tools/name_hash.py --table                     # dump the whole table
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def name_hash(name: str) -> int:
    h = 0
    for ch in name.encode("latin-1"):
        if ch == 0:
            break
        c = ch - 256 if ch >= 128 else ch
        h = ((h * 0x21) ^ (c & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


def _decompress(entry: str, out_dir: str) -> str:
    """Pull one datafile.big entry out and LZX-decompress it (see big-archive-format.md)."""
    big = os.path.join(ROOT, "assets/game/data/datafile.big")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools/big_list.py"), big,
                    "--extract", entry, "--out", out_dir],
                   check=True, stdout=subprocess.DEVNULL)
    packed = os.path.join(out_dir, entry)
    plain = packed + ".txt"
    subprocess.run([os.path.join(ROOT, "tools/big_decompress"), packed, plain],
                   check=True, stdout=subprocess.DEVNULL)
    return plain


def item_names():
    """Every `cItem*`/definition name in items.txt, plus the prop names beside them."""
    with tempfile.TemporaryDirectory() as td:
        text = open(_decompress("items.txt", td), "r", errors="replace").read()
    names = set()
    # `cItemDefinition Foo` / `cWeaponDefinition Foo` ... — the block headers.
    for m in re.finditer(r'^\s*c[A-Za-z0-9_]+\s+([A-Za-z0-9_\-]+)\s*$', text, re.M):
        names.add(m.group(1))
    # ... and every quoted value, which is where ItemName/PropName live.
    for m in re.finditer(r'"([A-Za-z0-9_\-]{2,64})"', text):
        names.add(m.group(1))
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--lookup", nargs="+", default=[], metavar="HEX")
    ap.add_argument("--table", action="store_true")
    a = ap.parse_args()

    for n in a.names:
        print(f"{name_hash(n):08X} {n}")

    if a.lookup or a.table:
        table = {}
        for n in sorted(item_names()):
            table.setdefault(name_hash(n), []).append(n)
        if a.table:
            for h in sorted(table):
                print(f"{h:08X} {' | '.join(table[h])}")
        for hx in a.lookup:
            h = int(hx, 16)
            print(f"{h:08X} {' | '.join(table.get(h, ['<not in items.txt>']))}")


if __name__ == "__main__":
    main()
