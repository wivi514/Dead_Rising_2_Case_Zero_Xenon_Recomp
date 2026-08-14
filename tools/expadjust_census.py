"""THE REAL exp_adjust CENSUS — walking the control flow, not scanning for shapes.

Part 40. CLAUDE.md's do-not-chase list carries "exp_adjust is declared but zero
everywhere", measured over the FABLE 2 bank and never repeated for Case Zero, while
XenosRecomp parses the field and nothing reads it (one occurrence in the whole source
tree: its own declaration in shader_code.h). That is gotcha 295's pattern, and 295 is
the one that hid the mip chain for 34 parts.

An earlier attempt at this scanned every dword triple for a vfetch-shaped word pair and
reported 234 non-zero of 607. That number was junk: the scan over-accepts ALU words by
construction, and a histogram from an over-accepting scanner is not a census. This
reuses tools/synth_shader_container.py's own CF walk — the parser the shader cache is
actually built with — so every instruction it reads is one the translator also reads.
"""
import sys, os, glob, struct, collections
sys.path.insert(0, os.path.expanduser('~/GithubRepo/Dead_Rising_2_Case_Zero_Xenon_Recomp/tools'))
import synth_shader_container as S

def vfetch_exp(data):
    """Same CF walk as parse_ucode, reporting the one field it does not keep."""
    n = len(data) // 4
    dw = struct.unpack(f">{n}I", data[: n * 4])
    execs, cf_end, i = [], n, 0
    while i + 2 < cf_end:
        for (w0, w1) in ((dw[i], dw[i + 1] & 0xFFFF),
                         (((dw[i + 1] >> 16) | (dw[i + 2] << 16)) & 0xFFFFFFFF, dw[i + 2] >> 16)):
            op = S.bits(w1, 12, 4)
            addr, cnt, seq = S.bits(w0, 0, 12), S.bits(w0, 12, 3), S.bits(w0, 16, 12)
            if op in (1, 2, 3, 4, 5, 6, 13, 14) and cnt and (addr, cnt, seq) not in execs:
                execs.append((addr, cnt, seq))
                cf_end = min(cf_end, addr * 3)
        i += 3
    out = []
    for (addr, cnt, seq) in sorted(execs):
        for k in range(cnt):
            base = (addr + k) * 3
            if base + 2 >= n:
                continue
            w0, w1 = dw[base], dw[base + 1]
            if not ((seq >> (k * 2)) & 1):
                continue
            if S.bits(w0, 0, 5) != 0:            # FetchOpcode::VertexFetch
                continue
            exp = S.bits(w1, 24, 6)
            out.append((S.bits(w1, 16, 6), exp - 64 if exp >= 32 else exp))
    return out

tot, hist, nz = 0, collections.Counter(), []
files = sorted(glob.glob(sys.argv[1] + '/vs_*.ucode'))
for p in files:
    for fmt, exp in vfetch_exp(open(p, 'rb').read()):
        tot += 1
        hist[exp] += 1
        if exp:
            nz.append((os.path.basename(p), fmt, exp))
print('%d vertex fetches across %d vertex shaders' % (tot, len(files)))
print('exp_adjust:', sorted(hist.items()))
for r in nz[:30]:
    print('   non-zero:', r)
