#!/usr/bin/env python3
"""CZ_HLSL_PATCH hook: paint Chuck's character pixel shaders distinct constant
colours to POSITIVELY IDENTIFY which one draws the hair (part-92 flicker hunt).

The 7938-vert mesh earlier assumed to be hair is his torso; greying ps_ea2cd381
/ ps_45109c37 turned the TORSO pale, leaving the hair brown. Those two share the
character VS c3d6d301 with a third shader, ps_522e2b16, never touched. This arm:
  ea2cd381, 45109c37 -> grey  (the known body, as before)
  522e2b16           -> MAGENTA (the suspect: if the hair turns magenta it is this)
A constant colour also cannot flicker, so a magenta hair that stops shimmering
confirms the defect is in this shader's shading, not coverage. argv1=path argv2=name.
"""
import sys
path, name = sys.argv[1], sys.argv[2]
GREY = {"ps_ea2cd381e6e3e3f6", "ps_45109c37a74df5c1"}
MAG  = {"ps_522e2b166969c4cd"}
if name not in GREY and name not in MAG:
    sys.exit(0)
src = open(path).read()
ANCHOR = "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"
if ANCHOR not in src:
    print(f"{name}: alpha-test clip anchor not found", file=sys.stderr)
    sys.exit(1)
col = "float3(0.5, 0.5, 0.5)" if name in GREY else "float3(1.0, 0.0, 1.0)"
open(path, "w").write(src.replace(ANCHOR, f"\toC0.xyz = {col}; // HAIR-ID diagnostic\n" + ANCHOR, 1))
