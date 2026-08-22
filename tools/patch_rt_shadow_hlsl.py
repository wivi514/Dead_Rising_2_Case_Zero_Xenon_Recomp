#!/usr/bin/env python3
"""ROUTE (B)'s SHADER PATCH: make an atlas-sampling pixel shader read OUR screen-space
shadow factor instead of the title's cascade atlas.

WHY THIS EXISTS
---------------
Part 64 proved the shadow atlas is where this title's shadow term reads, and then proved
route (a) — writing ray-traced depths INTO that atlas — cannot be correct: every receiver
inside the map is compared against itself and there is no receiver-side offset to apply
(docs/phase5-notes.md §6cv §7j). Route (b) computes the factor per RECEIVING PIXEL, where
a surface cannot shadow itself by construction, and has the shaders read that.

`tools/shadow_shader_census.py` says which shaders and which fetch slots — 126 shaders,
140 (shader, slot) pairs, measured against hardware's own register file in twenty `.xtr`
world traces, not guessed. This tool performs the substitution on XenosRecomp's HLSL,
before DXC, so the result is an ordinary second SPIR-V cache selected with
`CZ_SHADER_SPV` — the same same-binary-arm mechanism the part-33 NaN family and
`assets/shader_spv_pre45` use. **With RT off the runtime loads the stock cache and is
instruction-path identical to today.**

WHY THE SUBSTITUTION IS ONE RULE FOR ALL 140 USES
-------------------------------------------------
The census classified what each use does with the fetched value, and there are only two
shapes, both MONOTONIC and both SATURATING:

  pcf4 (116 uses)  four taps at exactly (-0.5,-0.5) (0.5,-0.5) (-0.5,0.5) (0.5,0.5),
                   each compared `> receiverDepth`, then weighted by `getWeights2D` on
                   the same slot;
  tap1 (24 uses)   one centre tap feeding `saturate((receiver - sampled) * k - bias)`.

So returning 1.0 at every atlas tap reads as LIT in both and 0.0 reads as OCCLUDED in
both, whatever the weighting — no per-shader special case, and no dependence on where in
the shader the compare happens to sit.

AND WHY THE WEIGHTS ARE PATCHED TOO — this is the part that buys SOFTNESS
-------------------------------------------------------------------------
A binary tap value would make the result binary however it is weighted, which would cap
route (b) at hard shadows. The weights are recoverable, though, and in a way that does
NOT depend on the shader: the emitted code builds the four bilinear products as
`w.<4swizzle> * w.<4swizzle>` out of components each of which is one of {a, b, 1-a, 1-b}
(the 1-x form is emitted as `pc(255).w - w.x`, pc255.w being the literal 1.0). A census
over the 116 pcf4 uses found THIRTEEN distinct swizzle pairings — so no single pattern
can be matched — but every one of them is a product of two of those four components.

Set a = b = 0.5 and all four components equal 0.5, so every product is 0.25 whatever the
swizzle. The shader then computes the plain MEAN of its four taps. Giving tap i the
threshold (i + 0.5)/4 against a continuous factor turns that mean into a 5-level
quantisation of it — genuinely soft edges, uniformly, with no knowledge of the swizzle.
`getWeights2D` returns float2, so returning `0.5.xx` is all that is needed.

The ceiling is stated rather than hidden: five levels, and the `tap1` family stays binary
because it has no filter to borrow. `CZ_VK_RT_HARD=1` makes the runtime write a 0/1
factor image, which collapses this to hard shadows in one arm without a rebuild.

USAGE (normally via build_shader_spv.sh's CZ_HLSL_PATCH hook)
    patch_rt_shadow_hlsl.py <file.hlsl> <shader_name> [--map config/rt_shadow_slots.json]
Exit 0 and "patched N tap(s), M weight call(s)" when it rewrote the file; exit 0 and
"not in the map" when the shader is not a shadow sampler; **exit 1 when the shader IS in
the map and the expected call sites are not all there**, because a silently unpatched
shader is a surface that keeps the old shadow with no line anywhere saying so.
"""
import argparse
import json
import os
import re
import sys

def rt_block_offset():
    """Where the RT block lives in the shared constants — READ OUT OF THE RENDERER.

    An offset written down twice is an offset that will disagree, and this one fails
    QUIETLY when it does: the shader would load a neighbouring word as a descriptor
    index, which is a valid index into the same bindless heap, so it samples a real but
    wrong texture instead of erroring. So it is not written down twice. The renderer
    declares it as an expression of the constants above it, which is exactly what the
    two arithmetic lines below evaluate; if either line stops matching, this raises
    rather than guessing.
    """
    src = open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            'runtime', 'gpu', 'vk_renderer.cpp')).read()
    m = re.search(r'kSharedClipPlanes\s*=\s*(\d+)\s*\+\s*(\d+)\s*\*\s*(\d+)\s*;', src)
    n = re.search(r'kSharedRtShadow\s*=\s*kSharedClipPlanes\s*\+\s*(\d+)\s*\*\s*(\d+)\s*;', src)
    if not m or not n:
        raise SystemExit('patch_rt_shadow_hlsl.py: cannot read kSharedRtShadow out of '
                         'runtime/gpu/vk_renderer.cpp — the shared-constant block was '
                         'restructured and this tool must be updated with it')
    return int(m.group(1)) + int(m.group(2)) * int(m.group(3)) + \
        int(n.group(1)) * int(n.group(2))


RT_BLOCK = rt_block_offset()

HELPERS = r'''
// ===================================================================================
// INJECTED BY tools/patch_rt_shadow_hlsl.py — route (b) screen-space RT shadows
// ===================================================================================
// This shader samples the title's shadow cascade atlas; the census
// (config/rt_shadow_slots.json) says at which fetch slot. Those taps have been
// redirected here, to a factor the renderer computed per RECEIVING PIXEL with a ray
// query. Read tools/patch_rt_shadow_hlsl.py's header for why one substitution serves
// every shader that does this.
#ifdef __spirv__
#define xeRtShadowIndex    vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + %(B)d)
#define xeRtShadowSampler  vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + %(B4)d)
#define xeRtShadowScaleU   vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + %(B8)d)
#define xeRtShadowScaleV   vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + %(B12)d)
#else
static const uint  xeRtShadowIndex = 0, xeRtShadowSampler = 0;
static const float xeRtShadowScaleU = 0.0, xeRtShadowScaleV = 0.0;
#endif

// One atlas tap, answered from the factor image. `tap` is the tap's index in the 2x2
// bilinear pattern (the centre tap of a tap1 shader is 0), which is what turns the
// shader's own averaging into a 5-level quantisation of a continuous factor.
//
// The factor image is at the RENDER resolution and `pos` is SV_Position, so the lookup
// is exact and needs no reprojection: this pixel's own screen position IS the address
// of this pixel's own shadow term. That is the whole reason route (b) cannot
// self-shadow the way route (a) did.
float4 xeRtShadowTap(float4 pos, uint tap)
{
    // Descriptor index 0 is the 1x1 white dummy. If the renderer has not published a
    // factor image this frame the tap reads white = LIT, i.e. the frame loses shadows
    // rather than gaining black ones — the honest failure, and one an engagement
    // counter on the renderer side is watching for anyway.
    float2 uv = float2(pos.x * xeRtShadowScaleU, pos.y * xeRtShadowScaleV);
    float f = g_Texture2DDescriptorHeap[xeRtShadowIndex].SampleLevel(
                  g_SamplerDescriptorHeap[xeRtShadowSampler], uv, 0.0).x;
    float t = (float(tap) + 0.5) * 0.25;
    // 1.0 is "no occluder": the pcf4 family compares `tap > receiverDepth` and 1.0 is
    // the far end of the depth range, and the tap1 family computes
    // saturate((receiver - tap) * k - bias), which 1.0 drives to zero shadow.
    return (f > t) ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
}

// The bilinear weights, forced to the value that makes every 2x2 product 0.25 whatever
// swizzle the shader pairs them in. See the tool header.
float2 xeRtShadowWeights() { return float2(0.5, 0.5); }
// ===================================================================================
''' % {'B': RT_BLOCK, 'B4': RT_BLOCK + 4, 'B8': RT_BLOCK + 8, 'B12': RT_BLOCK + 12}

# The tap index each emitted offset carries. The four are the 2x2 bilinear pattern and
# the order matches the order getWeights2D's products are built in, which is what makes
# the 0.25-each substitution give the plain mean.
TAP_OF_OFFSET = {('-0.5', '-0.5'): 0, ('0.5', '-0.5'): 1,
                 ('-0.5', '0.5'): 2, ('0.5', '0.5'): 3,
                 ('0', '0'): 0, ('0.0', '0.0'): 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('hlsl')
    ap.add_argument('name')
    ap.add_argument('--map', default=None)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mp = args.map or os.path.join(root, 'config', 'rt_shadow_slots.json')
    shaders = json.load(open(mp))['shaders']
    if args.name not in shaders:
        print('%s: not in the map' % args.name)
        return 0
    entry = shaders[args.name]
    slots = set(entry['slots'])
    shapes = {int(k): v for k, v in entry['shapes'].items()}

    src = open(args.hlsl).read()
    lines = src.splitlines(keepends=True)

    tap_re = re.compile(
        r'tfetch2D\(\s*s(\d+)_Texture2DDescriptorIndex\s*,\s*s\d+_SamplerDescriptorIndex\s*,'
        r'\s*[^,]+,\s*float2\(\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*\)\s*\)')
    w_re = re.compile(
        r'getWeights2D\(\s*s(\d+)_Texture2DDescriptorIndex\s*,\s*s\d+_SamplerDescriptorIndex\s*,'
        r'\s*[^,]+,\s*float2\([^)]*\)\s*\)')

    taps = weights = 0
    per_slot = {s: 0 for s in slots}

    def tap_sub(m):
        nonlocal taps
        slot = int(m.group(1))
        if slot not in slots:
            return m.group(0)
        off = (m.group(2), m.group(3))
        if off not in TAP_OF_OFFSET:
            raise SystemExit('%s: slot %d tap at an unmapped offset %s — the census said '
                             'this family only uses the 2x2 pattern and the centre; the '
                             'substitution would silently mis-index the dither'
                             % (args.name, slot, off))
        taps += 1
        per_slot[slot] += 1
        return 'xeRtShadowTap(iPos, %du)' % TAP_OF_OFFSET[off]

    def w_sub(m):
        nonlocal weights
        if int(m.group(1)) not in slots:
            return m.group(0)
        weights += 1
        return 'xeRtShadowWeights()'

    out = []
    for l in lines:
        l = tap_re.sub(tap_sub, l)
        l = w_re.sub(w_sub, l)
        out.append(l)
    body = ''.join(out)

    # The helpers go straight after the sampler heap declaration, which is the last
    # thing they depend on. Anchoring on that line rather than on a line number keeps
    # this working when XenosRecomp's preamble changes.
    anchor = 'SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);\n'
    if anchor not in body:
        print('%s: FAILED — sampler heap declaration not found; XenosRecomp\'s preamble '
              'changed and the helper has nowhere to go' % args.name, file=sys.stderr)
        return 1
    body = body.replace(anchor, anchor + HELPERS, 1)

    # THE GATE. A shader in the map whose call sites did not all rewrite is a surface
    # that keeps the title's own shadow while every counter says RT is on — exactly the
    # failure gotcha 386 describes, where a build measured a perfect fix because the
    # feature was silently inert. Loud, per shader, and non-zero.
    bad = []
    for s in sorted(slots):
        want = 4 if shapes.get(s) == 'pcf4' else 1
        if per_slot[s] != want:
            bad.append('slot %d: expected %d tap(s) for shape %s, rewrote %d'
                       % (s, want, shapes.get(s), per_slot[s]))
    if bad:
        print('%s: FAILED — %s' % (args.name, '; '.join(bad)), file=sys.stderr)
        return 1

    open(args.hlsl, 'w').write(body)
    print('%s: patched %d tap(s), %d weight call(s), slots %s'
          % (args.name, taps, weights, sorted(slots)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
