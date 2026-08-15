#!/usr/bin/env python3
"""Re-evaluate the per-zone COMMON_TEXTURE vs COMMON_TEXTURE_LOD decision against a
LIVE cz_runtime process, with the camera where it is RIGHT NOW.

WHY THIS EXISTS
---------------
Part 42/43 (item 00i): the engine picks a zone's common texture set ONCE, at zone
load, from the camera position of that instant (sub_82270870 -> sub_821C4F28 ->
sub_82175040), and nothing re-runs the choice while the player stands still. The
CZ_ZONE_TEX_PROBE hook prints the inputs at decision time; this tool answers the
complementary question — "what would the decision say NOW?" — without stopping the
game (process_vm_readv, not gdb: a ptrace stop during a streaming test contaminates
the test, see CLAUDE.md's live-recovery note).

Structures (derived in docs/phase5-notes.md §6bv and part 43):
  g            = [0x82A46294]              the world singleton
  camera       = floats at g+0x40..0x48    what sub_82175040 measures distance from
  level        = [g+0x34F5C]               indexes the threshold-boost tables
  world 'this' = passed to sub_82270870    (from the probe's rec/slot printout)
  slot         = [this + 0x834C + 4*zone]; rec = this + slot*0x3F0
  flag90C      = byte rec+0x90C  ("zone is LOD-capable": full set < 0x280000 bytes
                                  AND a COMMON_TEXTURE_LOD.tex exists)
  volObj       = [rec+0x910]; count=[+0x120]; elems=[+0x124], stride 0xD0
  per volume   : sphere (x,y,z,r) at e+0x80, skip bit0 of u32 e+0x90,
                 threshold float e+0xA8 (boosted by tables 0x82042C18/0x82042D68
                 when below the per-level cutoff)
  decision     : LOD iff flag90C and EVERY unskipped volume has
                 |cam-c| - 0.01 - r > threshold

Usage:
  tools/zone_lod_live.py <pid> <world_this_hex> [zone ...]
Reads the guest base from /proc/<pid>/maps is not possible (anonymous); instead
grep the run log for "guest memory at 0x" and pass it with --base, or let the tool
find the one 4 GiB anonymous mapping.
"""
import argparse
import ctypes
import ctypes.util
import struct
import sys

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


class iovec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]


def read_mem(pid, addr, size):
    buf = ctypes.create_string_buffer(size)
    local = iovec(ctypes.cast(buf, ctypes.c_void_p), size)
    remote = iovec(ctypes.c_void_p(addr), size)
    n = libc.process_vm_readv(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    if n != size:
        raise OSError(ctypes.get_errno(), f"read {size} at {addr:#x} -> {n}")
    return buf.raw


def find_guest_base(pid):
    # The flat 4 GiB guest map is the only mapping of that size.
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            rng = line.split()[0]
            lo, hi = (int(x, 16) for x in rng.split("-"))
            if hi - lo >= 0x100000000:
                return lo
    raise SystemExit("no 4 GiB mapping found; pass --base from the run log")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("world", help="the streaming world 'this' (hex), from the probe")
    ap.add_argument("zones", nargs="*", type=int, default=None)
    ap.add_argument("--base", type=lambda x: int(x, 16), default=None)
    args = ap.parse_args()

    base = args.base if args.base is not None else find_guest_base(args.pid)
    world = int(args.world, 16)
    pid = args.pid

    def u32(ga):
        return struct.unpack(">I", read_mem(pid, base + ga, 4))[0]

    def u8(ga):
        return read_mem(pid, base + ga, 1)[0]

    def f32(ga):
        return struct.unpack(">f", read_mem(pid, base + ga, 4))[0]

    g = u32(0x82A46294)
    cam = (f32(g + 0x40), f32(g + 0x44), f32(g + 0x48))
    level = u32(g + 0x34F5C)
    force = u8(0x82A57BD7)
    boost_below = f32(0x82042C18 + 4 * level) if level < 16 else 0.0
    boost_mul = f32(0x82042D68 + 4 * level) if level < 16 else 1.0
    print(f"g={g:08X} cam=({cam[0]:.2f},{cam[1]:.2f},{cam[2]:.2f}) level={level} "
          f"force={force} boost(<{boost_below:g} x{boost_mul:g})")

    zones = args.zones or list(range(9))
    for zone in zones:
        slot = u32(world + 0x834C + 4 * zone)
        if slot >= 0x1000:
            print(f"zone {zone}: slot {slot:#x} (unmapped)")
            continue
        rec = world + slot * 0x3F0
        flag = u8(rec + 0x90C)
        vol = u32(rec + 0x910)
        name = read_mem(pid, base + rec + 0x69C, 32).split(b"\0")[0].decode(
            "ascii", "replace")
        line = f"zone {zone} slot={slot} rec={rec:08X} name={name!r} flag90C={flag}"
        if not vol:
            print(line + " vol=NULL -> FULL")
            continue
        count = u32(vol + 0x120)
        elems = u32(vol + 0x124)
        near = far = skip = 0
        detail = []
        for i in range(min(count, 256)):
            e = elems + i * 0xD0
            x, y, z, r = struct.unpack(">4f", read_mem(pid, base + e + 0x80, 16))
            sk = u32(e + 0x90) & 1
            thr = f32(e + 0xA8)
            if thr < boost_below:
                thr *= boost_mul
            d = ((cam[0] - x) ** 2 + (cam[1] - y) ** 2 + (cam[2] - z) ** 2) ** 0.5 \
                - 0.01 - r
            if sk:
                skip += 1
                verdict = "SKIP->full"
            elif thr - d < 1.3e-11:
                far += 1
                verdict = "far"
            else:
                near += 1
                verdict = "NEAR->full"
            detail.append(f"  vol[{i}] c=({x:.1f},{y:.1f},{z:.1f}) r={r:.1f} "
                          f"thr={thr:.1f} d={d:.1f} {verdict}")
        lod = bool(force) or (flag and near == 0 and skip == 0 and count > 0)
        print(f"{line} count={count} near={near} far={far} skip={skip} -> "
              f"{'LOD' if lod else 'FULL'}")
        for ln in detail:
            print(ln)


if __name__ == "__main__":
    main()
