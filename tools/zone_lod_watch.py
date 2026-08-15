#!/usr/bin/env python3
"""Sample a live cz_runtime every N seconds: player/camera position, each zone's
state word in the streaming table at this+0x841C, and what the texture-set
decision would say from the current position.

WHY: part 43 measured ZERO texture-set re-decisions across a 9.5-minute
EXPLORER roam. That is only evidence of a dead promotion trigger if the roam
actually LEFT the spawn zones' volumes — this watcher records both halves
(where Chuck was, what each zone's state/verdict was) so the next such claim
carries its own control (gotcha 151: an arm with no counter cannot be shown to
have engaged).

Usage: tools/zone_lod_watch.py <pid> <world_hex> <base_hex> [interval] [count]
"""
import struct
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__file__))
from zone_lod_live import read_mem  # noqa: E402


def main():
    pid = int(sys.argv[1])
    world = int(sys.argv[2], 16)
    base = int(sys.argv[3], 16)
    interval = float(sys.argv[4]) if len(sys.argv) > 4 else 10.0
    count = int(sys.argv[5]) if len(sys.argv) > 5 else 60

    def u32(ga):
        return struct.unpack(">I", read_mem(pid, base + ga, 4))[0]

    def u8(ga):
        return read_mem(pid, base + ga, 1)[0]

    def f32(ga):
        return struct.unpack(">f", read_mem(pid, base + ga, 4))[0]

    for tick in range(count):
        try:
            g = u32(0x82A46294)
            cam = (f32(g + 0x40), f32(g + 0x44), f32(g + 0x48))
            level = u32(g + 0x34F5C)
            b_lo = f32(0x82042C18 + 4 * level) if level < 20 else 0.0
            b_mul = f32(0x82042D68 + 4 * level) if level < 20 else 1.0
            nzones = u32(world + 0x8614)
            inflight = u32(world + 0x8618)
            states = []
            verdicts = []
            for z in range(min(nzones, 12)):
                st = u32(world + 0x841C + z * 0x10)
                states.append(str(st))
                slot = u32(world + 0x834C + 4 * z)
                rec = world + slot * 0x3F0
                flag = u8(rec + 0x90C)
                vol = u32(rec + 0x910)
                if not vol or not flag:
                    verdicts.append("F")   # full (or no choice to make)
                    continue
                cnt = u32(vol + 0x120)
                elems = u32(vol + 0x124)
                near = 0
                minm = 1e9
                for i in range(min(cnt, 256)):
                    e = elems + i * 0xD0
                    x, y, zz, r = struct.unpack(
                        ">4f", read_mem(pid, base + e + 0x80, 16))
                    sk = u32(e + 0x90) & 1
                    thr = f32(e + 0xA8)
                    if thr < b_lo:
                        thr *= b_mul
                    d = ((cam[0] - x) ** 2 + (cam[1] - y) ** 2
                         + (cam[2] - zz) ** 2) ** 0.5 - 0.01 - r
                    m = d - thr
                    if sk or m < 0:
                        near += 1
                    if m < minm:
                        minm = m
                verdicts.append(f"{'F' if near else 'L'}({minm:.0f})")
            print(f"{time.strftime('%H:%M:%S')} cam=({cam[0]:.1f},{cam[1]:.1f},"
                  f"{cam[2]:.1f}) inflight={inflight} states={','.join(states)} "
                  f"would={' '.join(verdicts)}", flush=True)
        except OSError as e:
            print(f"read failed ({e}); process gone?", flush=True)
            return
        time.sleep(interval)


if __name__ == "__main__":
    main()
