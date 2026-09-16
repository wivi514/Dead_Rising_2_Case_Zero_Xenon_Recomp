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
    tools/guest_poke.py <pid> <host base hex> <guest va hex> u8:1              # one flag byte
    tools/guest_poke.py <pid> <host base hex> <guest va hex> read:16      # hex + f32 + f16

The host base is the `runtime: guest memory at 0x...` line of the run's log. The value
is written BIG-ENDIAN (guest byte order) and read back so a silent failure is visible.
`read:N` is the twin (part 120): N bytes via `process_vm_readv`, printed as hex, as
big-endian floats and as big-endian halves — the exposure global (0x829DEBF4) and a
16_FLOAT resolve destination are the two things it was written to watch.
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
    libc = ctypes.CDLL('libc.so.6', use_errno=True)

    class iovec(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('len', ctypes.c_size_t)]

    if kind == 'read':
        n = int(val)
        rb = ctypes.create_string_buffer(n)
        loc = iovec(ctypes.cast(rb, ctypes.c_void_p), n)
        rem = iovec(base + va, n)
        got = libc.process_vm_readv(pid, ctypes.byref(loc), 1, ctypes.byref(rem), 1, 0)
        if got != n:
            sys.exit(f'process_vm_readv read {got}: errno {ctypes.get_errno()}')
        b = rb.raw
        print(f'{va:08X}: {b.hex()}')
        if n >= 4:
            print('  f32 BE:', ' '.join(f'{struct.unpack(">f", b[i:i+4])[0]:.6g}' for i in range(0, n - 3, 4)))
        print('  f16 BE:', ' '.join(f'{struct.unpack(">e", b[i:i+2])[0]:.6g}' for i in range(0, n - 1, 2)))
        return
    # u8 exists because the debug-flag table is one byte per flag and a u32 poke of
    # `DISABLE TIME OF DAY` (0x82A57CAA) also zeroes its three neighbours.
    payload = (struct.pack('>f', float(val)) if kind == 'f32'
               else struct.pack('>B', int(val, 0)) if kind == 'u8'
               else struct.pack('>I', int(val, 0)))

    buf = ctypes.create_string_buffer(payload)
    loc = iovec(ctypes.cast(buf, ctypes.c_void_p), len(payload))
    rem = iovec(base + va, len(payload))
    n = libc.process_vm_writev(pid, ctypes.byref(loc), 1, ctypes.byref(rem), 1, 0)
    if n != len(payload):
        sys.exit(f'process_vm_writev wrote {n}: errno {ctypes.get_errno()}')
    rb = ctypes.create_string_buffer(len(payload))
    loc2 = iovec(ctypes.cast(rb, ctypes.c_void_p), len(payload))
    libc.process_vm_readv(pid, ctypes.byref(loc2), 1, ctypes.byref(rem), 1, 0)
    print(f'wrote {va:08X} = {payload.hex()}; read back {rb.raw.hex()}')


if __name__ == '__main__':
    main()
