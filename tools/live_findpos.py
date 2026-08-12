#!/usr/bin/env python3
"""Find the player's POSITION in guest memory by watching what moves when they walk.

WHY THIS EXISTS. Making a picture defect reproducible needs the player restored, not
just described, and that needs the offset holding their world position. Reading it out
of a struct we can name is the obvious route and it failed: the object CZ_AUTOCHUCK
steers (`g_gameDebugController + 0x30`) does not change ONE dword of its first 2 KB
while the player crosses the map, so it is a controller, not the transform holder.

So this binds the field the only way that cannot be fooled: sample the same memory
twice while the player MOVES, and keep the float triples that moved by a walking
distance. A single sample cannot distinguish a position from any other three floats
(the error that cost this project a symbol table); two samples plus a plausibility
window on the delta can, and a third sample kills the survivors that were noise.

Uses process_vm_readv, so the game is never ptrace-stopped -- an operator mid-walk
would otherwise be frozen for a second by a gdb attach, changing the very motion this
is measuring.

There is a second mode that needs NO motion at all, and it is the one to reach for
right after an F9. The .pose file gives the camera's world position exactly (the view
matrix's rows are orthonormal, so eye = -R^T t is not a fit but an identity), and in a
third-person game the player stands a few units from it -- so `near` scans the heap for
float triples close to a known point. In a crowd it is self-validating: a correct scan
finds MANY actor positions clustered near the camera, and a wrong reading of the matrix
finds none.

Usage:
  live_findpos.py <addr-hex> <bytes> [--samples N] [--interval S]
  live_findpos.py scan                 # the guest heap window, coarse
  live_findpos.py near <x> <y> <z> [radius]      # no motion needed

Prints surviving candidates as `+OFFSET (x, y, z) -> (x, y, z)  moved D`.
"""
import sys, os, re, time, ctypes, struct, glob

MIN_STEP, MAX_STEP = 0.05, 60.0        # a walking delta, in world units, per sample gap


def reader():
    pid = int(os.popen('pgrep -x cz_runtime').read().split()[0])
    logs = glob.glob(os.path.expanduser('~/DR2CZ-troubleshooting/**/*.log'), recursive=True)
    base = None
    for p in sorted(logs, key=os.path.getmtime, reverse=True):
        m = re.search(r'guest memory at (0x[0-9a-f]+)', open(p, errors='ignore').read(65536))
        if m:
            base = int(m.group(1), 16)
            break
    if base is None:
        sys.exit('no "runtime: guest memory at 0x..." line in any recent log')
    libc = ctypes.CDLL('libc.so.6', use_errno=True)

    class iovec(ctypes.Structure):
        _fields_ = [('iov_base', ctypes.c_void_p), ('iov_len', ctypes.c_size_t)]

    def read(gaddr, size):
        buf = ctypes.create_string_buffer(size)
        loc = iovec(ctypes.cast(buf, ctypes.c_void_p), size)
        rem = iovec(ctypes.c_void_p(base + gaddr), size)
        n = libc.process_vm_readv(pid, ctypes.byref(loc), 1, ctypes.byref(rem), 1, 0)
        return buf.raw if n == size else None
    return read, pid, base


def triples(buf, addr):
    """Every 4-aligned float triple in the buffer, big-endian (the guest's order)."""
    out = {}
    n = (len(buf) // 4) * 4
    vals = struct.unpack(f'>{n // 4}f', buf[:n])
    for i in range(len(vals) - 2):
        v = vals[i:i + 3]
        if all(abs(x) < 1e5 and (x == x) for x in v):     # finite, world-scaled
            out[addr + i * 4] = v
    return out


def near_mode(read, x, y, z, radius):
    import numpy as np
    want = np.array([x, y, z], dtype=np.float32)
    hits = []
    for r in range(0, 96):                       # the streaming heap window, 4 MB a step
        addr = 0xA0000000 + r * 0x400000
        b = read(addr, 0x400000)
        if not b:
            continue
        a = np.frombuffer(b, dtype='>f4').astype(np.float32)
        if a.size < 3:
            continue
        # Every 4-aligned triple: a[i], a[i+1], a[i+2]
        x0, y0, z0 = a[:-2], a[1:-1], a[2:]
        d = (x0 - want[0]) ** 2 + (y0 - want[1]) ** 2 + (z0 - want[2]) ** 2
        idx = np.nonzero(d <= radius * radius)[0]
        for i in idx:
            hits.append((addr + int(i) * 4, float(x0[i]), float(y0[i]), float(z0[i]),
                         float(np.sqrt(d[i]))))
    hits.sort(key=lambda h: h[4])
    print(f'{len(hits)} float triples within {radius} of '
          f'({x:.2f}, {y:.2f}, {z:.2f})')
    for h in hits[:40]:
        print(f'  {h[0]:08X}  ({h[1]:8.2f},{h[2]:8.2f},{h[3]:8.2f})  dist {h[4]:.2f}')
    if not hits:
        print('  NONE. Either that point is not where the camera was (the frame\'s first '
              'draw may be a SHADOW pass, whose "camera" is the light), or the heap '
              'window scanned is wrong.')
    return hits


def main():
    read, pid, base = reader()
    print(f'pid {pid}, guest base 0x{base:x}')
    if sys.argv[1] == 'near':
        x, y, z = (float(v) for v in sys.argv[2:5])
        radius = float(sys.argv[5]) if len(sys.argv) > 5 else 12.0
        near_mode(read, x, y, z, radius)
        return
    if sys.argv[1] == 'scan':
        # The guest heap window this title streams actors into, coarse pass. Chosen
        # from the addresses the pose capture reports (AAxxxxxx), widened either way.
        regions = [(0xA0000000 + i * 0x400000, 0x400000) for i in range(0, 64)]
    else:
        regions = [(int(sys.argv[1], 16), int(sys.argv[2], 0))]
    samples = int(sys.argv[sys.argv.index('--samples') + 1]) if '--samples' in sys.argv else 3
    interval = float(sys.argv[sys.argv.index('--interval') + 1]) if '--interval' in sys.argv else 2.0

    live = None
    for s in range(samples):
        if s:
            time.sleep(interval)
        cur = {}
        for addr, size in regions:
            b = read(addr, size)
            if b:
                cur.update(triples(b, addr))
        if live is None:
            live = {k: [v] for k, v in cur.items()}
            print(f'sample 0: {len(live)} candidate triples')
            continue
        nxt = {}
        for k, hist in live.items():
            if k not in cur:
                continue
            a, b2 = hist[-1], cur[k]
            d = sum((b2[i] - a[i]) ** 2 for i in range(3)) ** 0.5
            if MIN_STEP <= d <= MAX_STEP:
                nxt[k] = hist + [b2]
        live = nxt
        print(f'sample {s}: {len(live)} survive a {MIN_STEP}..{MAX_STEP} move')
        if not live:
            print('  nothing moved — is the player standing still?')
            return

    # Report the steadiest movers first: a real position moves by a similar amount each
    # interval, where noise jumps around.
    def score(h):
        ds = [sum((h[i + 1][j] - h[i][j]) ** 2 for j in range(3)) ** 0.5
              for i in range(len(h) - 1)]
        return (max(ds) - min(ds)) / (sum(ds) / len(ds) + 1e-9)
    for k in sorted(live, key=lambda k: score(live[k]))[:25]:
        h = live[k]
        path = ' -> '.join(f'({v[0]:.1f},{v[1]:.1f},{v[2]:.1f})' for v in h)
        print(f'  {k:08X}  {path}   steadiness {score(h):.2f}')


main()
