#!/usr/bin/env python3
"""Gas-station rooftop DECK gravel (part 96): patch ps_f20be39713a34795 — the shader
that actually paints the deck FLOOR (a tiled 1024x1024 fine gravel, s0=11696000) — to
output a chosen DIAGNOSTIC term instead of its final blended colour. Part 95 proved the
gravel texture is present and byte-identical to Xenia, the draw is issued with identical
geometry/state, yet our deck is smooth where Xenia's is fine gravel. This isolates WHY.

Reads the translated HLSL, writes the diagnostic HLSL. Mode via -D XE_F20_DIAG=N:
  1  frac(gravel UV) as R,G     -> BUSY rainbow = the UV tiles (fine); one smooth gradient
                                   = the UV maps ONCE (which would render smooth). THE fork.
  2  solid magenta              -> does f20be397 actually COVER the deck? (deck magenta=yes)
  3  raw s0 gravel sample.rgb   -> is the texture sampled sharp (gravel) or blurred (mip)?
  4  the gravel UV magnitude    -> |UV| scaled; shows the tiling RANGE.
Anchors are exact lines from XenosRecomp's output for this shader; re-derive if it moves."""
import sys
src=open(sys.argv[1]).read().split("\n"); out=[]
DONE={"cap":False,"emit":False}
for l in src:
    out.append(l)
    if l.strip()=="CubeMapData cubeMapData = (CubeMapData)0;":
        out.append("\tfloat2 XE_F20_UV = 0;")
    # capture the gravel UV right before the s0 fetch (first occurrence only)
    if not DONE["cap"] and "tfetch2D(s0_Texture2DDescriptorIndex" in l and "r0.xy" in l:
        # r0.xy is the gravel UV at this point — but we appended AFTER the line, so grab from the
        # line above by re-reading r0 is not possible; instead capture on the line before via edit:
        DONE["cap"]=True
txt="\n".join(out)
# insert the capture BEFORE the gravel sample line
sample='\t\t\tr2.xyzw = tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, r0.xy, float2(0, 0)).wxyz;'
txt=txt.replace(sample, "\t\t\tXE_F20_UV = r0.xy;\n"+sample, 1)
# override the output before the alpha-test clip
clip="\t\t\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"
ov=("#ifdef XE_F20_DIAG\n"
    "#if XE_F20_DIAG==1\n\t\t\toC0 = float4(frac(XE_F20_UV), 0.0, 1.0);\n"
    "#elif XE_F20_DIAG==2\n\t\t\toC0 = float4(1.0, 0.0, 1.0, 1.0);\n"
    "#elif XE_F20_DIAG==3\n\t\t\toC0 = float4(tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, XE_F20_UV, float2(0,0)).xyz, 1.0);\n"
    "#elif XE_F20_DIAG==4\n\t\t\toC0 = float4(frac(length(XE_F20_UV)).xxx, 1.0);\n"
    "#elif XE_F20_DIAG==5\n\t\t\toC0 = float4(frac(iPos.z*64.0), frac(iPos.z*512.0), iPos.z, 1.0);\n#endif\n#endif\n")
txt=txt.replace(clip, ov+clip, 1)
open(sys.argv[2],"w").write(txt)
for m in ["XE_F20_UV = r0.xy;","XE_F20_DIAG==1","XE_F20_DIAG==3"]:
    assert m in txt, "anchor missing: "+m
print("wrote",sys.argv[2])
