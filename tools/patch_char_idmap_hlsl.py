#!/usr/bin/env python3
"""CZ_HLSL_PATCH hook: paint EACH of Chuck's four character pixel shaders a distinct
constant colour so the operator can point at which one draws the hair. Part-92 flicker
hunt — the hair shader was misidentified twice (torso, watch), so this stops guessing.

  ps_ea2cd381 -> grey    (torso, already known)
  ps_45109c37 -> blue    (other body parts)
  ps_522e2b16 -> magenta (watch, already known)
  ps_ab3a6ccc -> green   (small specular+cube mesh: face or hair)

Whatever colour the HAIR is names its shader. If the hair stays brown, it uses a
shader outside this set (different VS) and we widen. A constant colour also cannot
flicker, so a coloured hair that goes solid confirms the defect is that shader's shading.
argv1=path argv2=name.
"""
import sys
path, name = sys.argv[1], sys.argv[2]
COL = {
    "ps_ea2cd381e6e3e3f6": "float3(0.5, 0.5, 0.5)",
    "ps_45109c37a74df5c1": "float3(0.0, 0.3, 1.0)",
    "ps_522e2b166969c4cd": "float3(1.0, 0.0, 1.0)",
    "ps_ab3a6ccc2b0b5fca": "float3(0.0, 1.0, 0.0)",
}
if name not in COL:
    sys.exit(0)
src = open(path).read()
ANCHOR = "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"
if ANCHOR not in src:
    print(f"{name}: alpha-test clip anchor not found", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(ANCHOR, f"\toC0.xyz = {COL[name]}; // CHAR-IDMAP\n" + ANCHOR, 1))
