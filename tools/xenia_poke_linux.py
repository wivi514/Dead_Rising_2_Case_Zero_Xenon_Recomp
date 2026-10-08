#!/usr/bin/env python3
"""Hold the title's own debug bools ON inside a RUNNING Xenia under Wine on Linux.

WHY THIS EXISTS (2026-10-08). A picture comparison against Xenia (open item 0zf: does a
soldier zombie keep his helmet up close?) means standing next to zombies in Xenia, and
there Chuck dies. Our runtime has `CZ_DEBUG_FLAGS` for exactly this ("ZOMBIES IGNORE ALL
HUMANS", "CHUCK GOD MODE", "DISABLE DEATH SEQUENCE"), but those are bytes in the title's
debug-bool table, so the same writes work in Xenia's guest memory too.
`tools/xenia_poke.py` does it on Windows through kernel32; when Xenia runs locally under
Wine it is an ordinary Linux process, so process_vm_readv/writev do it with no ptrace stop.

The guest base is FOUND, not assumed, as in xenia_poke.py: `gFinalGammaParameters` sits at
0x820B40C8 in this XEX, and the first power-of-two base (2^32 .. 2^40) where that string
reads back is the mapping. Each flag is re-asserted every 100 ms because the title clears
them on a level load (our runtime counts the same clears).

    tools/xenia_poke_linux.py safe            # god mode + no death + zombies ignore humans
    tools/xenia_poke_linux.py safe --once     # write once and exit
    tools/xenia_poke_linux.py off             # clear the three flags and exit

The addresses are the ones `CZ_DEBUG_FLAGS` resolves by label in this XEX (its log prints
them: "'ZOMBIES IGNORE ALL HUMANS' @82A57C63").
"""
import ctypes
import os
import subprocess
import sys
import time

FLAGS = {
    'CHUCK GOD MODE': 0x82A57C61,
    'ZOMBIES IGNORE ALL HUMANS': 0x82A57C63,
    'DISABLE DEATH SEQUENCE': 0x82A57C75,
}
ANCHOR_VA = 0x820B40C8
ANCHOR = b'gFinalGammaParameters'

libc = ctypes.CDLL(None, use_errno=True)


class Iov(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('len', ctypes.c_size_t)]


def read(pid, addr, n):
    buf = ctypes.create_string_buffer(n)
    local, remote = Iov(ctypes.addressof(buf), n), Iov(addr, n)
    got = libc.process_vm_readv(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    return buf.raw[:got] if got > 0 else b''


def write(pid, addr, data):
    buf = ctypes.create_string_buffer(data, len(data))
    local, remote = Iov(ctypes.addressof(buf), len(data)), Iov(addr, len(data))
    return libc.process_vm_writev(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)


def find_xenia():
    # By the process NAME, not `pgrep -f`: a shell whose command line mentions the exe
    # matches -f too, and the first version poked a bash.
    for d in os.listdir('/proc'):
        if d.isdigit():
            try:
                if open(f'/proc/{d}/comm').read().strip().lower().startswith('xenia_canary'):
                    return int(d)
            except OSError:
                pass
    return None


def find_base(pid):
    for shift in range(32, 41):
        base = 1 << shift
        if read(pid, base + ANCHOR_VA, len(ANCHOR)) == ANCHOR:
            return base
    return None


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'safe'
    once = '--once' in sys.argv
    # WAIT for Xenia and for the XEX to be mapped (up to 10 minutes), so this can be
    # started alongside the emulator instead of after it.
    pid = base = None
    for _ in range(6000):
        pid = pid if pid and os.path.exists(f'/proc/{pid}') else find_xenia()
        base = find_base(pid) if pid else None
        if base is not None:
            break
        time.sleep(0.1)
    if base is None:
        sys.exit('no xenia_canary.exe with the Case Zero XEX mapped after 10 minutes')
    value = b'\x00' if mode == 'off' else b'\x01'
    print(f'xenia pid {pid}, guest base {base:#x}; {"clearing" if mode == "off" else "holding ON"}: '
          + ', '.join(FLAGS))
    sets = 0
    while True:
        for name, va in FLAGS.items():
            if read(pid, base + va, 1) != value:
                if write(pid, base + va, value) != 1:
                    sys.exit(f'write to {name} @{va:08X} failed (errno {ctypes.get_errno()})')
                sets += 1
                print(f'  {name} @{va:08X} set ({sets} writes so far)', flush=True)
        if once or mode == 'off':
            return
        time.sleep(0.1)
        if not os.path.exists(f'/proc/{pid}'):
            print('xenia exited')
            return


if __name__ == '__main__':
    main()
