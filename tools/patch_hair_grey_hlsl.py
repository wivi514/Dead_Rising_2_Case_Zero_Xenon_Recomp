#!/usr/bin/env python3
"""CZ_HLSL_PATCH hook: force Chuck's two LIT hair pixel shaders to output flat grey,
keeping their alpha cutout. Diagnostic for the part-92 hair-flicker hunt.

A constant colour can only flicker if the set of pixels the hair COVERS changes
frame to frame (geometry/depth/coverage). If grey Chuck is solid, the flicker is
in the shading math; if grey Chuck's silhouette shimmers, it is coverage. Every
other pixel shader passes through untouched, so only Chuck's hair turns grey.

argv[1]=HLSL path, argv[2]=shader name. Edits in place. A targeted shader that
lacks the clip anchor is a FAILURE so the build names it.
"""
import sys
path, name = sys.argv[1], sys.argv[2]
TARGETS = {"ps_ea2cd381e6e3e3f6", "ps_45109c37a74df5c1"}
if name not in TARGETS:
    sys.exit(0)
src = open(path).read()
ANCHOR = "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"
if ANCHOR not in src:
    print(f"{name}: alpha-test clip anchor not found", file=sys.stderr)
    sys.exit(1)
# oC0.w (alpha) is left as the shader computed it so the cutout is unchanged;
# only rgb is overwritten.
INJECT = "\toC0.xyz = float3(0.5, 0.5, 0.5); // HAIR-GREY diagnostic\n"
open(path, "w").write(src.replace(ANCHOR, INJECT + ANCHOR, 1))
