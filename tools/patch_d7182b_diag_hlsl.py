#!/usr/bin/env python3
"""Gas-station rooftop deck (part 96): patch ps_d7182b2fb8f8c474 — the 32x32 surface that
part 93 saw render the deck PURE BLACK and part 94 injected gravel into. Part 95 found the
deck FLOOR is actually ps_f20be397 (a tiled fine gravel), present and correct in our build,
and the hypothesis is that d7182b COVERS it. This tests that directly. Mode via -D XE_D71_DIAG:
  1  DISCARD every d7182b pixel (clip(-1))  -> if the deck now shows f20be397 fine gravel,
                                               d7182b was covering it: HYPOTHESIS CONFIRMED.
  2  solid MAGENTA                          -> shows exactly WHERE d7182b covers the screen.
Anchor is the exact alpha-test line from XenosRecomp's output; re-derive if it moves."""
import sys
src=open(sys.argv[1]).read()
anchor="\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"
inject=("#ifdef XE_D71_DIAG\n"
        "#if XE_D71_DIAG==1\n\tclip(-1.0);\n"
        "#elif XE_D71_DIAG==2\n\toC0 = float4(1.0, 0.0, 1.0, 1.0);\n"
        "#elif XE_D71_DIAG==3\n\toC0 = float4(frac(iPos.z*64.0), frac(iPos.z*512.0), iPos.z, 1.0);\n#endif\n#endif\n")
assert anchor in src, "anchor missing"
src=src.replace(anchor, inject+anchor, 1)
open(sys.argv[2],"w").write(src)
assert "XE_D71_DIAG==1" in src
print("wrote",sys.argv[2])
