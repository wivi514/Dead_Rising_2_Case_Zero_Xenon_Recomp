#!/usr/bin/env python3
"""Pin the TIME OF DAY (or write any guest global) inside a RUNNING Xenia on Windows.

WHY THIS EXISTS (part 119, 2026-09-16). "Interiors are really dark, especially at
night" needs a hardware NIGHT frame, and every capture on disk is daytime. Reaching night
legitimately means an hour of play (Katey's Zombrex, then the clock past 19:00); poking
the mission clock ends the game at the deadline. But the title's own debug flag
`DISABLE TIME OF DAY` (byte 0x82A57CAA) makes the lighting read a pinned hour from the
float at 0x82A578D0 instead of the clock — the same two globals `CZ_DEBUG_FLAGS` and
`tools/guest_poke.py` drive in our runtime — and the title's code does the rest. So the
same poke, into Xenia's guest memory, gives a night frame at any spot in five minutes,
and the operator only has to stand there and press F4.

HOW. Xenia maps the guest's 4 GiB at a power-of-two host base (`memory.cc`: the first
of 2^32..2^63 that maps; in practice 0x100000000), so guest VA `v` lives at `base + v`.
`WriteProcessMemory` from the same user needs no privilege. The base is FOUND, not
assumed: the string `gFinalGammaParameters` sits at 0x820B40C8 in this XEX, and the
first candidate base where ReadProcessMemory returns it is the mapping. The flag byte is
RE-ASSERTED every 100 ms because the game clears it (our runtime does the same, and
counts it); `--once` writes once and exits.

    python xenia_poke.py hour 22          # pin the lighting to 22:00, hold the flag ON
    python xenia_poke.py hour 22 --once   # write and exit (the game may clear the flag)
    python xenia_poke.py off              # release the flag (the clock drives again)
    python xenia_poke.py read             # print the flag, the pinned hour, the gamma
    python xenia_poke.py f32 829EDCF4 1.5 # any float; u32 likewise
    python xenia_poke.py clock 18 55      # set the MISSION clock (hour, minute) — the
                                          # heap object is found by its vtable + shape;
                                          # 19:00 is Katey's deadline AND the hour the
                                          # title's IsNight() (sub_82160450) flips on

Windows only (ctypes over kernel32). Only `clock` touches the mission clock — and past 19:00 with Katey undosed the game ends.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_OPERATION = 0x0008
PROCESS_QUERY_INFORMATION = 0x0400

FLAG_DISABLE_TIME_OF_DAY = 0x82A57CAA   # byte; the title's debug bool
PINNED_HOUR = 0x82A578D0                # float, hours 0..24, read when the flag is set
GAMMA_METER = 0x829EDCF4                # float, the Visuals gamma meter's value
CLOCK_VTABLE = 0x820112C0               # the mission clock object's vtable (found live)
CLOCK_DAY, CLOCK_HOUR, CLOCK_MIN, CLOCK_SEC = 0x14, 0x18, 0x1C, 0x20
ANCHOR_VA = 0x820B40C8
ANCHOR = b'gFinalGammaParameters'

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = (wt.DWORD, wt.BOOL, wt.DWORD)
k32.ReadProcessMemory.argtypes = (wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t))
k32.WriteProcessMemory.argtypes = (wt.HANDLE, wt.LPVOID, wt.LPCVOID, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t))


def find_pid():
    import subprocess
    out = subprocess.run(['tasklist', '/fo', 'csv', '/nh'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        cells = [c.strip('"') for c in line.split('","')]
        if cells and cells[0].lower().startswith('xenia'):
            return int(cells[1])
    sys.exit('no xenia*.exe process; launch Xenia first')


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def write(h, addr, data):
    put = ctypes.c_size_t(0)
    ok = k32.WriteProcessMemory(h, ctypes.c_void_p(addr), data, len(data), ctypes.byref(put))
    if not ok or put.value != len(data):
        sys.exit(f'WriteProcessMemory({addr:#x}) failed: error {ctypes.get_last_error()}')


def find_base(h):
    for n in range(32, 48):
        base = 1 << n
        got = read(h, base + ANCHOR_VA, len(ANCHOR))
        if got == ANCHOR:
            return base
    sys.exit('guest mapping not found: no candidate base holds the XEX anchor string '
             '(is this Case Zero, and is it past the loader?)')


def find_clock(h, base):
    """The mission clock lives on the title's heap (0xA0000000.. in our runtime; anywhere
    the kernel handed out in Xenia), so scan every readable 4 MB of the guest for the
    vtable followed by day/hour/min/sec in range, then confirm the seconds ADVANCE."""
    vt = struct.pack('>I', CLOCK_VTABLE)
    cands = []
    for va in range(0, 0x100000000, 1 << 22):
        chunk = read(h, base + va, 1 << 22)
        if not chunk:
            continue
        i = chunk.find(vt)
        while i != -1:
            if i % 4 == 0 and i + 0x24 <= len(chunk):
                d, hr, mn, sc = struct.unpack_from('>4I', chunk, i + CLOCK_DAY)
                if d <= 40 and hr < 24 and mn < 60 and sc < 60:
                    cands.append(va + i)
            i = chunk.find(vt, i + 4)
    if not cands:
        sys.exit('no clock-shaped object with the expected vtable; is the game in play?')
    before = {c: read(h, base + c + CLOCK_DAY, 16) for c in cands}
    time.sleep(2.5)
    live = [c for c in cands if read(h, base + c + CLOCK_DAY, 16) != before[c]]
    if len(live) != 1:
        print(f'{len(cands)} candidates, {len(live)} advancing: '
              + ' '.join(f'{c:08X}' for c in cands))
        if not live:
            sys.exit('none advanced in 2.5 s (paused? a menu?)')
    return live[0]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pid = find_pid()
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                        PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        sys.exit(f'OpenProcess({pid}) failed: error {ctypes.get_last_error()}')
    base = find_base(h)
    print(f'xenia pid {pid}, guest base {base:#x}')
    cmd = sys.argv[1]

    def show():
        flag = read(h, base + FLAG_DISABLE_TIME_OF_DAY, 1)[0]
        hour = struct.unpack('>f', read(h, base + PINNED_HOUR, 4))[0]
        gamma = struct.unpack('>f', read(h, base + GAMMA_METER, 4))[0]
        print(f'DISABLE TIME OF DAY={flag} pinned hour={hour:.2f} gamma meter={gamma:.3f}')

    if cmd == 'read':
        show()
    elif cmd == 'off':
        write(h, base + FLAG_DISABLE_TIME_OF_DAY, b'\x00')
        show()
    elif cmd == 'hour':
        hour = float(sys.argv[2])
        write(h, base + PINNED_HOUR, struct.pack('>f', hour))
        write(h, base + FLAG_DISABLE_TIME_OF_DAY, b'\x01')
        show()
        if '--once' in sys.argv:
            return
        print('holding the flag ON (Ctrl-C to stop; the pin stays until `off`)')
        reasserts = 0
        while True:
            time.sleep(0.1)
            cur = read(h, base + FLAG_DISABLE_TIME_OF_DAY, 1)
            if cur is None:
                sys.exit('xenia went away')
            if cur[0] != 1:
                write(h, base + FLAG_DISABLE_TIME_OF_DAY, b'\x01')
                reasserts += 1
                print(f'flag re-asserted ({reasserts})')
    elif cmd == 'clock':
        clk = find_clock(h, base)
        d, hr, mn, sc = struct.unpack('>4I', read(h, base + clk + CLOCK_DAY, 16))
        print(f'mission clock at {clk:08X}: day {d} {hr:02d}:{mn:02d}:{sc:02d}')
        if len(sys.argv) >= 4:
            write(h, base + clk + CLOCK_HOUR, struct.pack('>I', int(sys.argv[2])))
            write(h, base + clk + CLOCK_MIN, struct.pack('>I', int(sys.argv[3])))
            d, hr, mn, sc = struct.unpack('>4I', read(h, base + clk + CLOCK_DAY, 16))
            print(f'set: day {d} {hr:02d}:{mn:02d}:{sc:02d}')
    elif cmd in ('f32', 'u32'):
        va = int(sys.argv[2], 16)
        val = struct.pack('>f', float(sys.argv[3])) if cmd == 'f32' \
            else struct.pack('>I', int(sys.argv[3], 0))
        write(h, base + va, val)
        print(f'wrote {va:08X} = {val.hex()}; read back {read(h, base + va, 4).hex()}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
