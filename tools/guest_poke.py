#!/usr/bin/env python3
"""Write one dword into a LIVE guest's memory, without stopping it.

WHY THIS EXISTS (part 119). The question "does this guest global do anything to the
picture" is answered fastest by changing the global while the game runs and reading the
frame stats — no hook, no rebuild, no arm. `process_vm_writev` does it without ptrace,
so the game never stalls (gotcha 7: a probe that stalls the game manufactures what it
reports). It settled the Visuals gamma constant in one park: 0x829EDCF4 poked to 2.0,
0.5 and back moved the safehouse's mean luma 26.1 / 26.0 / 25.1 / 25.3 — nothing
(phase5-notes §6fb §5).

    tools/guest_poke.py <pid> <host base hex> <guest va hex> f32:2.0
    tools/guest_poke.py <pid> <host base hex> <guest va hex> u32:0x3F800000

The host base is the `runtime: guest memory at 0x...` line of the run's log. The value
is written BIG-ENDIAN (guest byte order) and read back so a silent failure is visible.
"""
import ctypes
import struct
import sys


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    pid = int(sys.argv[1])
    base = int(sys.argv[2], 16)
    va = int(sys.argv[3], 16)
    kind, val = sys.argv[4].split(':', 1)
    payload = struct.pack('>f', float(val)) if kind == 'f32' else struct.pack('>I', int(val, 0))
    libc = ctypes.CDLL('libc.so.6', use_errno=True)

    class iovec(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('len', ctypes.c_size_t)]

    buf = ctypes.create_string_buffer(payload)
    loc = iovec(ctypes.cast(buf, ctypes.c_void_p), len(payload))
    rem = iovec(base + va, len(payload))
    n = libc.process_vm_writev(pid, ctypes.byref(loc), 1, ctypes.byref(rem), 1, 0)
    if n != len(payload):
        sys.exit(f'process_vm_writev wrote {n}: errno {ctypes.get_errno()}')
    rb = ctypes.create_string_buffer(4)
    loc2 = iovec(ctypes.cast(rb, ctypes.c_void_p), 4)
    libc.process_vm_readv(pid, ctypes.byref(loc2), 1, ctypes.byref(rem), 1, 0)
    print(f'wrote {va:08X} = {payload.hex()}; read back {rb.raw.hex()}')


if __name__ == '__main__':
    main()
