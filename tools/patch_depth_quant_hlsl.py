#!/usr/bin/env python3
"""CZ_HLSL_PATCH hook: emulate Xenos 24-bit FLOAT depth quantization in every VS.

Why it exists: the part-92 Chuck-hair flicker investigation exonerated every
renderer arm and every draw input, leaving depth-test tie behaviour as the last
per-pixel per-frame variable. Hardware's D24F depth has ~8x coarser quantization
than our D24_UNORM at this title's scene depths, so near-coplanar hair layers
that TIE on hardware (resolving stably by draw order under LESS_EQUAL) are
strictly ordered for us and the winner re-rolls with every animation step.
This patch truncates the vertex z/w to a 20-bit mantissa (float24's), restoring
hardware's tie population. Diagnostic arm, selected as a second cache via
CZ_SHADER_SPV; the unpatched cache is the control.

Interface (build_shader_spv.sh): argv[1] = HLSL path, argv[2] = shader name.
Edits in place. Pixel shaders pass through untouched. A VS without the anchor
is a FAILURE (exit 1) so the build names it rather than shipping it unpatched.
"""
import sys

path, name = sys.argv[1], sys.argv[2]
if not name.startswith("vs_"):
    sys.exit(0)

ANCHOR = "\toPos.xy += g_HalfPixelOffset * oPos.w;\n"
QUANT = ("\t// XE depth-quant arm: truncate z/w to float24's 20-bit mantissa so\n"
         "\t// near-coplanar layers tie exactly as they do on Xenos D24F.\n"
         "\tif (oPos.w != 0.0) { float xe_zq = oPos.z / oPos.w;"
         " oPos.z = asfloat(asuint(xe_zq) & 0xFFFFFFF8u) * oPos.w; }\n")

src = open(path).read()
if ANCHOR not in src:
    print(f"{name}: position epilogue anchor not found", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(ANCHOR, ANCHOR + QUANT, 1))
