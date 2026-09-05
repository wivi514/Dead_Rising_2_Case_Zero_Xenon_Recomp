#!/usr/bin/env python3
"""Black gas-station rooftop (part 93/94): patch the ONE material that renders the
recessed rooftop floor flat (ps_4796cb371c33e1e2) so it outputs a chosen shading TERM
instead of its final colour. The pit shows the shader's constant fog/ambient floor
(26,6,4) => the draw runs but its lit term is ~0 everywhere; this exposes which term.

Reads the translated HLSL on stdin path arg, writes the diagnostic HLSL. Selection is a
compile-time -D XE_ROOF_DIAG=N (1 albedo.rgb, 2 albedo.a, 3 lighting r6, 4 shadow, 5 green).
Anchors are exact source lines from XenosRecomp's output for this shader; if the shader is
re-translated and the lines move, re-derive them (grep the four markers below)."""
import sys
src=open(sys.argv[1]).read().split("\n"); out=[]
for i,l in enumerate(src,1):
    out.append(l)
    if l.strip()=="CubeMapData cubeMapData = (CubeMapData)0;":
        out.append("\tfloat3 XE_DBG_ALB=0; float XE_DBG_A=0; float3 XE_DBG_LIGHT=0; float XE_DBG_SHADOW=0;")
    if "tfetch2D(s0_Texture2DDescriptorIndex" in l:      # albedo sampled as .wxyz: a=.x rgb=.yzw
        out.append("\t\t\tXE_DBG_ALB = r1.yzw; XE_DBG_A = r1.x;")
    if l.strip()=="r0.y = dot(r2.xywz, r0.wzxy);":       # PCF shadow visibility 0..1
        out.append("\t\t\tXE_DBG_SHADOW = r0.y;")
txt="\n".join(out)
txt=txt.replace("\t\t\tr0.xyz = r4.xyz * r6.xyz;",
                "\t\t\tXE_DBG_LIGHT = r6.xyz;\n\t\t\tr0.xyz = r4.xyz * r6.xyz;",1)
clip="\t\t\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)"
ov=("#ifdef XE_ROOF_DIAG\n#if XE_ROOF_DIAG==1\n\t\t\toC0 = float4(saturate(XE_DBG_ALB), 1.0);\n"
    "#elif XE_ROOF_DIAG==2\n\t\t\toC0 = float4(XE_DBG_A.xxx, 1.0);\n"
    "#elif XE_ROOF_DIAG==3\n\t\t\toC0 = float4(saturate(XE_DBG_LIGHT), 1.0);\n"
    "#elif XE_ROOF_DIAG==4\n\t\t\toC0 = float4(XE_DBG_SHADOW.xxx, 1.0);\n"
    "#elif XE_ROOF_DIAG==5\n\t\t\toC0 = float4(0.0, 1.0, 0.0, 1.0);\n#endif\n#endif\n")
txt=txt.replace(clip, ov+clip, 1)
open(sys.argv[2],"w").write(txt)
for m in ["XE_DBG_ALB = r1.yzw","XE_DBG_SHADOW = r0.y","XE_DBG_LIGHT = r6.xyz","XE_ROOF_DIAG==5"]:
    assert m in txt, "anchor missing: "+m
print("wrote",sys.argv[2])
