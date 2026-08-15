#!/usr/bin/env python3
"""HARDWARE's PIXEL-SHADER ALU CONSTANTS for a named draw, with provenance per register.

WHY THIS EXISTS
---------------
The white-surface chain (`docs/open-items.md` 00f/00g) ends at an epilogue every one of
the 48 emitting shaders shares:

    r1.xyz = saturate(-c * pc(14).www + pc(255).www)      // saturate(1 - c*E)
    r0.xyz = c * pc(253).yyy
    r0.xyz = r0.xyz * pc(14).www + pc(253).xxx            // A*c*E + B
    r0.xyz = max(r0.xyz, pc(255).www)
    r0.xyz = -r1.xyz * r1.xyz + r0.xyz
    r0.xyz = r0.xyz * pc(254).zzz                         // * C
    oC0    = sqrt(abs(r0))

Every term in it except the colour is a CONSTANT the title uploads, and this project has
never read one of their values. Part 27 inferred the shape of the curve from a paint
probe instead ("no max takes its floor, therefore `c = 1/pc(14).w`"), and that inference
is only valid if `pc(253).y` is non-zero — if it is zero the max's argument does not
depend on the colour at all and the probe result is uninformative. One number decides
which of those two worlds we are in, and the capture has it.

`tools/xtr_draw_bindings.py` already replays the register file correctly, including the
`LOAD_ALU_CONSTANT` path that carries most of this title's constants (gotcha 262). What
it does not do is print the ALU constant file, because it was built to answer a question
about textures. This is that tool's replay loop with a different read-out.

PROVENANCE IS PART OF THE OUTPUT, NOT A FOOTNOTE
------------------------------------------------
81 of `w1_spawn`'s 620 `LOAD_ALU_CONSTANT` packets read memory the trace never recorded
(gotcha 263), so a replayed constant can be one of three things, and they must not print
the same way:

    set          written by a packet whose data the trace carries      -> trust it
    unset        no packet in this trace ever wrote it                 -> NOT zero
    UNRECOVERABLE a load pointed at memory the capture does not hold   -> NOT stale

A tool that prints 0.0 for the second and third cases invents hardware behaviour, which
is exactly the failure that made this project believe `c255.w` was 0 when the shader uses
it as its literal 1.0.

USAGE
    xtr_draw_constants.py <trace.xtr> --ps <hash> [--regs 14,18,19,253,254,255]
    xtr_draw_constants.py <trace.xtr> --ps <hash> --all      # every set register
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from xtr_draw_bindings import (BANKS, DRAW_OPCODES, Memory, decompress,  # noqa: E402
                               fnv1a)

BE = struct.Struct(">I")
ALU_BASE = 0x4000
SQ_VS_CONST = 0x2307
SQ_PS_CONST = 0x2308

# Provenance codes, in increasing order of "do not believe the number".
SET, UNSET, UNREC = 'set', 'unset', 'UNRECOVERABLE'


def f32(word):
    return struct.unpack('>f', struct.pack('>I', word))[0]


def fmt_vec(regs, prov, idx):
    """One `pc(idx)` as four floats, or a named absence. `idx` is already absolute."""
    out = []
    for c in range(4):
        r = idx + c
        p = prov.get(r, UNSET)
        if p is SET:
            out.append('%12.6f' % f32(regs[r]))
        else:
            out.append('%12s' % p)
    return ' '.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--ps', help='pixel shader hash (with or without the ps_ prefix)')
    ap.add_argument('--regs', default='14,18,19,253,254,255',
                    help='comma-separated pixel constant indices to print')
    ap.add_argument('--all', action='store_true',
                    help='print every pixel constant with any set component')
    ap.add_argument('--draws', type=int, default=1, help='how many matching draws')
    args = ap.parse_args()

    want_ps = args.ps.replace('ps_', '') if args.ps else None
    want = [int(x) for x in args.regs.split(',') if x != '']

    data, hdr = xtr.open_trace(args.trace)
    print('%s  version %s  title %s  %.1f MB'
          % (args.trace, hdr['version'], hdr['title'], hdr['size'] / 1e6))

    mem = Memory()
    regs = {}
    prov = {}
    bound = {0: None, 1: None}
    shown = 0
    unrec_loads = 0
    loads = 0

    def write(reg, value, how):
        regs[reg] = value
        prov[reg] = how

    for off, cmd in xtr.walk(data, len(data)):
        if cmd in (xtr.CMD_MEMORY_READ, xtr.CMD_MEMORY_WRITE):
            base, enc, elen, dlen = struct.unpack_from('<IIII', data, off + 4)
            try:
                mem.add(base, decompress(data[off + 20:off + 20 + elen], enc, dlen))
            except Exception as e:
                print('  [memory record at %08X undecodable: %s]' % (base, e),
                      file=sys.stderr)
            continue
        if cmd != xtr.CMD_PACKET_START:
            continue
        count = struct.unpack_from('<I', data, off + 8)[0]
        if not count:
            continue
        header = BE.unpack_from(data, off + 12)[0]

        def word(i):
            return BE.unpack_from(data, off + 12 + 4 * i)[0]

        ptype = header >> 30
        if ptype == 0:
            reg = header & 0x7FFF
            one = (header >> 15) & 1
            for i in range(count - 1):
                write(reg if one else reg + i, word(1 + i), SET)
            continue
        if ptype == 1:
            if count >= 3:
                write(header & 0x7FF, word(1), SET)
                write((header >> 11) & 0x7FF, word(2), SET)
            continue
        if ptype != 3:
            continue
        opcode = (header >> 8) & 0x7F

        if opcode == 0x2D and count >= 2:
            base_reg = BANKS.get((word(1) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(1) & 0x7FF
                for i in range(2, count):
                    write(base_reg + idx + i - 2, word(i), SET)
        elif opcode in (0x55, 0x56) and count >= 2:
            idx = word(1) & 0xFFFF
            for i in range(2, count):
                write(idx + i - 2, word(i), SET)
        elif opcode == 0x2F and count >= 4:
            base_reg = BANKS.get((word(2) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(2) & 0x7FF
                src = word(1) & 0x3FFFFFFC
                size = word(3) & 0xFFF
                loads += 1
                blob = mem.read(src, size * 4)
                if blob:
                    for i in range(size):
                        write(base_reg + idx + i, BE.unpack_from(blob, i * 4)[0], SET)
                else:
                    # The load HAPPENED on hardware; we simply cannot say with what.
                    # Marking the destinations rather than leaving the previous value
                    # is the whole point (gotcha 263). The SOURCE ADDRESS is carried in
                    # the marker: an unrecoverable register's next question is always
                    # "what memory was that, and who writes it" — part 42 asked it of
                    # the DoF passes' pc(252..255), where the answer decides whether
                    # the title computes those constants on the GPU (a resolve
                    # destination) or the CPU (our runtime's own memory to inspect).
                    unrec_loads += 1
                    for i in range(size):
                        write(base_reg + idx + i, 0,
                              '%s@%08X' % (UNREC, src + 4 * i))
        elif opcode == 0x27 and count >= 3:
            bound[word(1) & 3] = (word(1) & ~3, word(2) & 0xFFFF)
        elif opcode == 0x2B and count >= 3:
            size = word(2) & 0xFFFF
            bound[word(1) & 3] = ('inline', b''.join(
                struct.pack('>I', word(3 + i)) for i in range(min(size, count - 3))))
        elif opcode in DRAW_OPCODES and count >= 2:
            b = bound[1]
            if b is None:
                continue
            if b[0] == 'inline':
                name = '%016x' % fnv1a(b[1])
            else:
                code = mem.read(b[0], b[1] * 4)
                if not code:
                    continue
                name = '%016x' % fnv1a(code)
            if want_ps and name != want_ps:
                continue
            init_at = 2 if opcode == 0x22 else 1
            if count <= init_at:
                continue
            ps_base = (regs.get(SQ_PS_CONST, 256 << 0) & 0x1FF)
            if SQ_PS_CONST not in prov:
                ps_base = 256      # the value our renderer assumes when nothing said
            vs_base = regs.get(SQ_VS_CONST, 0) & 0x1FF
            print('\ndraw #%d  ps_%s  indices=%d  SQ_PS_CONST base=%d (vs base=%d)'
                  % (shown, name, word(init_at) >> 16, ps_base, vs_base))
            idxs = want
            if args.all:
                idxs = sorted({(r - ALU_BASE - ps_base * 4) // 4
                               for r in prov
                               if ALU_BASE + ps_base * 4 <= r < ALU_BASE + ps_base * 4 + 1024})
            for i in idxs:
                r = ALU_BASE + ps_base * 4 + i * 4
                print('  pc(%3d)  %s' % (i, fmt_vec(regs, prov, r)))
            shown += 1
            if shown >= args.draws:
                break

    print('\nLOAD_ALU_CONSTANT packets replayed: %d, of which %d read memory this trace '
          'does not carry' % (loads, unrec_loads))
    if not shown:
        print('no draw matched', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
