#!/usr/bin/env python3
"""Disassemble Direct3D 9 shader model 2/3 bytecode (ps_3_0 / vs_3_0) — for reading
Dead Rising 2 PC's shaders as the oracle for the Xbox 360 build's post chain.

WHY THIS EXISTS (part 120). The same engine shipped on PC with its shaders as plain
DX9 bytecode carrying constant tables and, in the Steam build, a DBUG chunk with the
HLSL variable names. Where the 360 bank holds Xenos microcode whose translation we own
and cannot fully trust as an oracle, the PC shader for the same `.bcp` name is an
INDEPENDENT statement of the same math (`LuminanceToExposure.bcp`, `LumAvgFinal.bcp`,
the tone map) — readable in twenty lines of assembly. No DX9 disassembler exists on this
Linux box, and the format is small: 32-bit tokens, one instruction token followed by
one destination and N source parameter tokens.

    python3 tools/d3d9_disasm.py <file.po|.vo|raw bytecode>

Scans for the version token (0xFFFF0300 / 0xFFFE0300 / 0x0200 variants) inside the
object, so a `.po` wrapper with a DBUG chunk in front is fine. Prints the CTAB constant
table first (name -> register), then the instructions with register names substituted.
Unknown opcodes are printed as `op_XX` with their token count, never skipped silently.
"""
import struct
import sys

OPS = {
    0: 'nop', 1: 'mov', 2: 'add', 3: 'sub', 4: 'mad', 5: 'mul', 6: 'rcp', 7: 'rsq',
    8: 'dp3', 9: 'dp4', 10: 'min', 11: 'max', 12: 'slt', 13: 'sge', 14: 'exp',
    15: 'log', 16: 'lit', 17: 'dst', 18: 'lrp', 19: 'frc', 20: 'm4x4', 21: 'm4x3',
    22: 'm3x4', 23: 'm3x3', 24: 'm3x2', 25: 'call', 26: 'callnz', 27: 'loop',
    28: 'ret', 29: 'endloop', 30: 'label', 31: 'dcl', 32: 'pow', 33: 'crs',
    34: 'sgn', 35: 'abs', 36: 'nrm', 37: 'sincos', 38: 'rep', 39: 'endrep',
    40: 'if', 41: 'ifc', 42: 'else', 43: 'endif', 44: 'break', 45: 'breakc',
    46: 'mova', 47: 'defb', 48: 'defi',
    64: 'texcoord', 65: 'texkill', 66: 'texld', 67: 'texbem', 68: 'texbeml',
    69: 'texreg2ar', 70: 'texreg2gb', 71: 'texm3x2pad', 72: 'texm3x2tex',
    73: 'texm3x3pad', 74: 'texm3x3tex', 76: 'texm3x3spec', 77: 'texm3x3vspec',
    78: 'expp', 79: 'logp', 80: 'cnd', 81: 'def', 82: 'texreg2rgb', 83: 'texdp3tex',
    84: 'texm3x2depth', 85: 'texdp3', 86: 'texm3x3', 87: 'texdepth', 88: 'cmp',
    89: 'bem', 90: 'dp2add', 91: 'dsx', 92: 'dsy', 93: 'texldd', 94: 'setp',
    95: 'texldl', 96: 'breakp',
    0xFFFE: 'comment', 0xFFFF: 'end',
}
CMP = {0: '', 1: '_gt', 2: '_eq', 3: '_ge', 4: '_lt', 5: '_ne', 6: '_le'}
REGTYPES = {0: 'r', 1: 'v', 2: 'c', 3: 't', 4: 'oPos', 5: 'oFog', 6: 'oPts', 7: 'oC', 8: 'oD',
            9: 'oT', 10: 's', 11: 'l', 12: 'i', 13: 'b', 14: 'aL', 15: 'oD', 16: 'p',
            17: 'a', 18: 'oPos', 19: 'oD', 20: 'oT'}  # 20+ overlap; enough for ps_3_0


def regname(tok):
    num = tok & 0x7FF
    rt = ((tok >> 28) & 7) | ((tok >> 9) & 0x18)
    if rt == 16 and (tok & 0x1800) == 0:  # r
        pass
    name = REGTYPES.get(rt, f'reg{rt}')
    return f'{name}{num}'


def srcstr(tok, names):
    base = regname(tok)
    base = names.get(base, base)
    sw = (tok >> 16) & 0xFF
    comps = ''.join('xyzw'[(sw >> (2 * i)) & 3] for i in range(4))
    if comps != 'xyzw':
        if comps[0] * 4 == comps:
            comps = comps[0]
        base += '.' + comps
    mod = (tok >> 24) & 0xF
    mods = {0: '{}', 1: '-{}', 2: '{}_bias', 3: '-{}_bias', 4: '{}_bx2', 5: '-{}_bx2',
            6: '1-{}', 7: '{}_x2', 8: '-{}_x2', 9: '{}_dz', 10: '{}_dw', 11: '|{}|',
            12: '-|{}|', 13: '!{}'}
    return mods.get(mod, '{}?' + str(mod)).format(base)


def dststr(tok, names):
    base = regname(tok)
    base = names.get(base, base)
    mask = (tok >> 16) & 0xF
    m = ''.join(c for i, c in enumerate('xyzw') if mask & (1 << i))
    mods = (tok >> 20) & 0xF
    s = base + ('.' + m if m and m != 'xyzw' else '')
    if mods & 1:
        s = s + '_sat'
    if mods & 2:
        s = s + '_pp'
    return s


def parse_ctab(data):
    """Return {register: name} out of the CTAB comment block, if any."""
    names = {}
    i = data.find(b'CTAB')
    if i < 0:
        return names
    base = i + 4
    size, creator, version, nconst, coff, flags, target = struct.unpack_from('<7I', data, base)
    for k in range(nconst):
        off = base + coff + k * 20
        nameoff, regset, regidx, regcount, _typ, _def = struct.unpack_from('<IHHHII', data, off)
        name = data[base + nameoff:data.index(b'\0', base + nameoff)].decode('latin1')
        pfx = {0: 'b', 1: 'i', 2: 'c', 3: 's'}.get(regset, '?')
        for r in range(regcount):
            names[f'{pfx}{regidx + r}'] = name if regcount == 1 else f'{name}[{r}]'
    return names


def disasm(data):
    # find the version token
    start = -1
    for i in range(0, len(data) - 4, 4):
        v, = struct.unpack_from('<I', data, i)
        if (v & 0xFFFF0000) in (0xFFFF0000, 0xFFFE0000) and (v & 0xFFFF) in (0x0200, 0x0300, 0x0201, 0x0101):
            start = i
            break
    if start < 0:
        raise SystemExit('no shader version token found')
    names = parse_ctab(data[start:])
    print('; constants:', ', '.join(f'{r}={n}' for r, n in sorted(names.items())))
    v, = struct.unpack_from('<I', data, start)
    print(f"{'ps' if v>>16==0xFFFF else 'vs'}_{(v>>8)&0xFF}_{v&0xFF}")
    i = start + 4
    while i + 4 <= len(data):
        tok, = struct.unpack_from('<I', data, i)
        op = tok & 0xFFFF
        if op == 0xFFFF:
            print('end')
            break
        if op == 0xFFFE:
            n = (tok >> 16) & 0x7FFF
            i += 4 + 4 * n
            continue
        n = (tok >> 24) & 0xF
        ctl = (tok >> 16) & 0xFF
        name = OPS.get(op, f'op_{op}')
        params = [struct.unpack_from('<I', data, i + 4 + 4 * k)[0] for k in range(n)]
        i += 4 + 4 * n
        if name == 'def':
            f = struct.unpack('<4f', struct.pack('<4I', *params[1:5]))
            print(f'def {dststr(params[0], names)}, {f[0]:g}, {f[1]:g}, {f[2]:g}, {f[3]:g}')
            continue
        if name == 'dcl':
            usage = params[0]
            reg = dststr(params[1], names)
            tt = {2: '_2d', 3: '_cube', 4: '_volume'}.get((usage >> 27) & 0xF, '')
            print(f'dcl{tt} {reg}   ; usage={usage & 0x1F} idx={(usage >> 16) & 0xF}')
            continue
        if name in ('ifc', 'breakc', 'setp'):
            name += CMP.get(ctl & 7, '')
        args = []
        if params:
            args.append(dststr(params[0], names) if name not in ('if', 'ifc', 'breakc', 'texkill', 'loop', 'rep', 'call', 'callnz', 'label') else srcstr(params[0], names))
            for p in params[1:]:
                args.append(srcstr(p, names))
        print(f'{name} ' + ', '.join(args))


if __name__ == '__main__':
    disasm(open(sys.argv[1], 'rb').read())
