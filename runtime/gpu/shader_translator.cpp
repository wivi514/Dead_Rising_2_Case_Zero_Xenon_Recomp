// Release D.2 — in-process shader translation. See shader_translator.h for why.
//
// STRUCTURE, in pipeline order, each section naming the Python it ports:
//
//   1. ucode analysis        port of tools/synth_shader_container.py parse_ucode()
//   2. container synthesis   port of build_ctab() / build_shader_struct() / synthesize()
//   3. HLSL                  XenosRecomp's own ShaderRecompiler, compiled into this
//                            binary from the sibling checkout (MIT) — NOT a port
//   4. ALU-const census      port of tools/alu_const_census.py census_file()
//   5. sidecar JSON          replicates Python json.dump(meta, indent=1) byte for byte
//   6. SPIR-V                DXC via its C API, dlopen'd, same argument list as the CLI
//
// THE PORTS ARE DELIBERATE DUPLICATES AND THE GATE IS WHAT KEEPS THEM HONEST. The
// Python pipeline built the 449-module cache this project has played the whole game
// on; this file must reproduce every one of those bytes exactly, and
// `cz_runtime --translate-shaders` against assets/shader_spv is the check (run it
// after any change to EITHER side). That is why the JSON writer mimics Python's
// indent=1 formatting down to the empty-list special case, and why the census scans
// the HLSL text rather than the SPIR-V — DXC folds constant indices into raw address
// math, so the HLSL is the only substrate that states every `vc(N)` read literally
// (tools/alu_const_census.py's docstring is the full argument).
//
// The sidecar is LOAD-BEARING FOR CORRECTNESS, not just speed: a register missing
// from aluConsts is one the runtime will not copy, and the shader then reads garbage.
// The byte-identity gate is what lets this port inherit the Python parser's standing
// verification arms (CZ_VK_VERIFY_CONST_GATHER and its poison) without re-running them.
#include "xenos_pch.h"

#ifdef _WIN32
// dxcapi.h needs the COM base types, and win_compat.h's WIN32_LEAN_AND_MEAN (correct
// for everything else in this runtime) is exactly what excludes them from windows.h —
// so IUnknown and IStream have to be pulled in by name or every interface in dxcapi.h
// fails with "expected class name".
#include <windows.h>
// win_compat.h #undefs `far`/`near` to free the identifiers for this runtime's own
// code — correct everywhere else, but windef.h defines FAR as `far`, so the COM
// headers below (which still spell `LPMALLOC FAR * ppMalloc`) would expand it to a
// stray identifier and every parameter list carrying it fails with "expected ')'".
// Re-point the uppercase macros at nothing for this TU.
#undef FAR
#define FAR
#undef NEAR
#define NEAR
#include <unknwn.h>
#include <objidl.h>
#else
#include <dlfcn.h>
#endif
#include <dxcapi.h>

#include "shader_recompiler.h" // XenosRecomp's, via this TU's private include path
#include "shader_translator.h"
#include "../host/host_paths.h"
#include "cz_shader_common_embedded.h" // generated at configure time; see CMakeLists

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <mutex>
#include <thread>

namespace ShaderTranslator
{
namespace
{
static inline uint32_t bits(uint32_t v, uint32_t lo, uint32_t n)
{
    return (v >> lo) & ((1u << n) - 1u);
}

// ---------------------------------------------------------------------------
// 1. ucode analysis — port of synth_shader_container.py parse_ucode().
//
// The Python file carries the full derivation for every non-obvious decision here
// (per-component liveness after the part-45 white-surface class, indirect vertex
// fetch after Fable 2 §3s, the scalar co-issue's RetainPrev gate) — those comments
// are not repeated; this is a transcription, and the transcription being exact is
// the property the gate checks.

struct VFetch
{
    uint32_t slot = 0, fetchSlot = 0, format = 0, sgn = 0, integer = 0;
    uint32_t strideDwords = 0, offsetDwords = 0, srcReg = 0, srcSwz = 0;
    int indirect = 0, addrAttr = -1;
};

struct UcodeInfo
{
    std::vector<VFetch> vfetch;           // program order
    std::vector<uint32_t> tfetchConsts;   // unique, program order
    uint32_t tfetchDim[32] = {};          // fetch-constant index -> 0=1D 1=2D 2=3D 3=cube
    std::set<uint32_t> inputs;            // PS interpolator inputs (read-before-write)
    std::set<uint32_t> exports;           // VS export indices / PS color+depth exports
};

static bool ParseUcode(const uint8_t* data, size_t size, UcodeInfo& u, std::string& err)
{
    const size_t n = size / 4;
    std::vector<uint32_t> dw(n);
    for (size_t i = 0; i < n; i++)
        dw[i] = (uint32_t(data[i * 4]) << 24) | (uint32_t(data[i * 4 + 1]) << 16) |
                (uint32_t(data[i * 4 + 2]) << 8) | uint32_t(data[i * 4 + 3]);

    // Walk the whole CF region (conditional flow puts more Exec blocks after the first
    // ExecEnd); the region ends where the lowest exec target begins. Two 48-bit CF
    // instructions per 3 dwords.
    struct Exec { uint32_t addr, cnt, seq; };
    std::vector<Exec> execs;
    size_t cfEnd = n;
    for (size_t i = 0; i + 2 < cfEnd; i += 3)
    {
        const uint32_t half[2][2] = {
            { dw[i], dw[i + 1] & 0xFFFFu },
            { (dw[i + 1] >> 16) | (dw[i + 2] << 16), dw[i + 2] >> 16 },
        };
        for (auto& h : half)
        {
            const uint32_t w0 = h[0], w1 = h[1];
            const uint32_t op = bits(w1, 12, 4);
            const uint32_t addr = bits(w0, 0, 12), cnt = bits(w0, 12, 3),
                           seq = bits(w0, 16, 12);
            if (((op >= 1 && op <= 6) || op == 13 || op == 14) && cnt) // Exec family
            {
                bool dup = false;
                for (auto& e : execs)
                    if (e.addr == addr && e.cnt == cnt && e.seq == seq) { dup = true; break; }
                if (!dup)
                    execs.push_back({ addr, cnt, seq });
                cfEnd = std::min(cfEnd, size_t(addr) * 3);
            }
        }
    }
    std::sort(execs.begin(), execs.end(), [](const Exec& a, const Exec& b) {
        return std::tie(a.addr, a.cnt, a.seq) < std::tie(b.addr, b.cnt, b.seq);
    });

    uint8_t written[64] = {};  // bit c = component c definitely written
    bool seenTf[32] = {};
    uint64_t vidRegs = 1;      // bit r = register r still holds the auto vertex index
    int regProducer[64];
    for (auto& r : regProducer) r = -1;

    auto noteRead = [&](uint32_t reg, uint8_t compMask) {
        if (u.inputs.count(reg))
            return;
        if (compMask & uint8_t(~written[reg]))
            u.inputs.insert(reg);
    };

    for (auto& e : execs)
    {
        for (uint32_t k = 0; k < e.cnt; k++)
        {
            const size_t base = size_t(e.addr + k) * 3;
            if (base + 2 >= n)
            {
                err = "instruction slot out of range (not microcode?)";
                return false;
            }
            const uint32_t w0 = dw[base], w1 = dw[base + 1], w2 = dw[base + 2];
            if ((e.seq >> (k * 2)) & 1) // fetch
            {
                const uint32_t fop = bits(w0, 0, 5);
                // Source-address read, per component: a vfetch address is ONE component
                // (its 2-bit srcSwizzle names it); a tfetch reads as many coordinate
                // components as its declared dimension.
                if (fop == 0)
                    noteRead(bits(w0, 5, 6), uint8_t(1u << bits(w0, 30, 2)));
                else if (fop == 1)
                {
                    static const int NCOMP[4] = { 1, 2, 3, 3 };
                    const int ncomp = NCOMP[bits(w2, 14, 2)];
                    const uint32_t swz = bits(w0, 26, 6);
                    uint8_t m = 0;
                    for (int c = 0; c < ncomp; c++)
                        m |= uint8_t(1u << ((swz >> (2 * c)) & 3));
                    noteRead(bits(w0, 5, 6), m);
                }
                else
                    noteRead(bits(w0, 5, 6), 0xF);

                if (fop == 0)
                {
                    VFetch vf;
                    vf.slot = e.addr + k;
                    vf.fetchSlot = bits(w0, 20, 5) * 3 + bits(w0, 25, 2);
                    vf.format = bits(w1, 16, 6);
                    vf.sgn = bits(w1, 12, 1);
                    vf.integer = bits(w1, 13, 1);
                    vf.strideDwords = bits(w2, 0, 8);
                    vf.offsetDwords = bits(w2, 8, 23);
                    // mini-fetch: shares the previous full fetch's buffer + stride
                    if (vf.strideDwords == 0 && !u.vfetch.empty())
                    {
                        vf.strideDwords = u.vfetch.back().strideDwords;
                        vf.fetchSlot = u.vfetch.back().fetchSlot;
                    }
                    const uint32_t src = bits(w0, 5, 6);
                    vf.srcReg = src;
                    vf.srcSwz = bits(w0, 30, 2);
                    vf.indirect = int(!((vidRegs >> src) & 1));
                    vf.addrAttr = vf.indirect ? regProducer[src] : -1;
                    u.vfetch.push_back(vf);
                    regProducer[bits(w0, 12, 6)] = int(u.vfetch.size()) - 1;
                }
                else if (fop == 1)
                {
                    const uint32_t c = bits(w0, 20, 5);
                    if (!seenTf[c])
                    {
                        seenTf[c] = true;
                        u.tfetchConsts.push_back(c);
                    }
                    u.tfetchDim[c] = bits(w2, 14, 2);
                }
                // Any write kills the vertex id in that register — including a fetch
                // writing its own source.
                vidRegs &= ~(1ull << bits(w0, 12, 6));
                // Destination swizzle: 3 bits per component, 7 = Keep (not a write);
                // a predicated fetch is not a definite write.
                if (!bits(w1, 31, 1))
                {
                    const uint32_t dswz = bits(w1, 0, 12);
                    uint8_t m = 0;
                    for (int c = 0; c < 4; c++)
                        if (((dswz >> (3 * c)) & 7) != 7)
                            m |= uint8_t(1u << c);
                    written[bits(w0, 12, 6)] |= m;
                }
            }
            else // ALU
            {
                const uint32_t vdst = bits(w0, 0, 6), sdst = bits(w0, 8, 6);
                const uint32_t exp = bits(w0, 15, 1);
                const uint32_t vmask = bits(w0, 16, 4), smask = bits(w0, 20, 4);
                const uint32_t predicated = bits(w1, 28, 1), vop = bits(w2, 24, 5);
                if (!exp)
                {
                    vidRegs &= ~(1ull << vdst);
                    regProducer[vdst] = -1;
                }
                // Lanes the vector op consumes: the dot family (and cube/max4/setp/kill)
                // reads fixed lane counts regardless of the write mask.
                int lanes[4];
                int nLanes = 0;
                if (vop == 15 || vop == 18 || vop == 19 || vop == 28 ||
                    (vop >= 20 && vop <= 27))
                {
                    lanes[0] = 0; lanes[1] = 1; lanes[2] = 2; lanes[3] = 3; nLanes = 4;
                }
                else if (vop == 16) { lanes[0] = 0; lanes[1] = 1; lanes[2] = 2; nLanes = 3; }
                else if (vop == 17) { lanes[0] = 0; lanes[1] = 1; nLanes = 2; }
                else
                {
                    for (int i2 = 0; i2 < 4; i2++)
                        if ((vmask >> i2) & 1)
                            lanes[nLanes++] = i2;
                    if (nLanes == 0) { lanes[0] = 0; nLanes = 1; } // XenosRecomp's .x fallback
                }
                const uint32_t swz1 = bits(w1, 16, 8), swz2 = bits(w1, 8, 8),
                               swz3 = bits(w1, 0, 8);
                auto laneMask = [&](uint32_t swz) -> uint8_t {
                    uint8_t m = 0;
                    for (int j = 0; j < nLanes; j++)
                        m |= uint8_t(1u << (((swz >> (2 * lanes[j])) + lanes[j]) & 3));
                    return m;
                };
                if (bits(w2, 31, 1))
                    noteRead(bits(w2, 16, 8) & 0x3F, laneMask(swz1));
                if (bits(w2, 30, 1))
                    noteRead(bits(w2, 8, 8) & 0x3F, laneMask(swz2));
                if (bits(w2, 29, 1))
                {
                    // src3 is shared: the VECTOR op reads it only for the 3-source
                    // opcodes; the SCALAR co-issue reads it whenever the scalar opcode
                    // is not RetainPrev (50), the idle-slot marker.
                    uint8_t m = 0;
                    if (vop == 11 || vop == 12 || vop == 13 || vop == 14) // Mad, Cnd*
                        m |= laneMask(swz3);
                    else if (vop == 17) // Dp2Add reads src3.x only
                        m |= uint8_t(1u << (swz3 & 3));
                    if (bits(w0, 26, 6) != 50) // scalar co-issue present
                    {
                        m |= uint8_t(1u << (((swz3 >> 6) + 3) & 3));
                        m |= uint8_t(1u << (swz3 & 3));
                    }
                    if (m)
                        noteRead(bits(w2, 0, 8) & 0x3F, m);
                }
                if (exp)
                    u.exports.insert(vdst); // both pipes write the export named by vectorDest
                else if (!predicated)
                {
                    written[vdst] |= uint8_t(vmask);
                    if (smask)
                        written[sdst] |= uint8_t(smask);
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 2. container synthesis — port of build_ctab() / build_shader_struct() /
// synthesize()'s container assembly. Everything big-endian: this fabricates the
// 0x102A11xx D3D container ShaderRecompiler parses.

struct Blob
{
    std::vector<uint8_t> b;
    void u32(std::initializer_list<uint32_t> vs)
    {
        for (uint32_t v : vs)
        {
            b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16));
            b.push_back(uint8_t(v >> 8));  b.push_back(uint8_t(v));
        }
    }
    void u16(std::initializer_list<uint32_t> vs)
    {
        for (uint32_t v : vs)
        {
            b.push_back(uint8_t(v >> 8));
            b.push_back(uint8_t(v));
        }
    }
    void raw(const uint8_t* d, size_t len) { b.insert(b.end(), d, d + len); }
};

static std::vector<uint8_t> BuildCtab(bool isVs, const std::vector<uint32_t>& tfSorted)
{
    // One full-range float4 array ('vc'/'pc') so every ALU constant reference
    // resolves, plus one sampler entry per tfetch fetch-constant.
    struct C { std::string name; uint32_t regset, regidx, regcount; };
    std::vector<C> consts;
    consts.push_back({ isVs ? "vc" : "pc", 2, 0, 256 });
    for (uint32_t c : tfSorted)
        consts.push_back({ "s" + std::to_string(c), 3, c, 1 });

    const uint32_t n = uint32_t(consts.size());
    const uint32_t tableSize = 7 * 4;
    const uint32_t infoOff = tableSize;
    const uint32_t typeinfoOff = infoOff + n * 0x14;
    const uint32_t namesOff = typeinfoOff + n * 0x10;

    std::vector<uint8_t> names;
    std::vector<uint32_t> nameOffs;
    for (auto& c : consts)
    {
        nameOffs.push_back(namesOff + uint32_t(names.size()));
        names.insert(names.end(), c.name.begin(), c.name.end());
        names.push_back(0);
    }
    while (names.size() % 4)
        names.push_back(0);

    Blob ct;
    const uint32_t total = namesOff + uint32_t(names.size());
    // ConstantTable {size, creator, version, constants, constantInfo, flags, target}
    ct.u32({ tableSize, 0, 0, n, infoOff, 0, 0 });
    for (uint32_t i = 0; i < n; i++)
    {
        ct.u32({ nameOffs[i] });
        ct.u16({ consts[i].regset, consts[i].regidx, consts[i].regcount, 0 });
        ct.u32({ typeinfoOff + i * 0x10, 0 }); // typeInfo, defaultValue
    }
    for (auto& c : consts)
    {
        if (c.regset == 2)
            ct.u16({ 1, 3, 1, 4, c.regcount, 0 }); // Vector, Float, 1x4, elements
        else
            ct.u16({ 4, c.regset == 3 ? 21u : 3u, 1, 1, 1, 0 }); // Object, Sampler2D
        ct.u32({ 0 });
    }
    ct.raw(names.data(), names.size());
    assert(ct.b.size() == total);

    Blob out;
    out.u32({ total + 4 }); // ConstantTableContainer.size
    out.raw(ct.b.data(), ct.b.size());
    return out.b;
}

static constexpr uint32_t POSITION = 0, TEXCOORD = 5;

static std::vector<uint8_t> BuildShaderStruct(bool isVs, uint32_t ucodeLen,
                                              const UcodeInfo& u)
{
    Blob sh;
    if (isVs)
    {
        std::vector<uint32_t> interps;
        for (uint32_t e : u.exports)
            if (e < 16)
                interps.push_back(e); // std::set iterates sorted
        // Dependent fetches are NOT declared as vertex elements (§3z) — XenosRecomp
        // emits them as in-shader raw loads. Original enumeration kept, so declared
        // elements' usage indices are unchanged.
        std::vector<std::pair<int, const VFetch*>> declared;
        for (size_t i = 0; i < u.vfetch.size(); i++)
            if (!u.vfetch[i].indirect)
                declared.push_back({ int(i), &u.vfetch[i] });
        // Shader {physicalOffset, size, field8, fieldC, field10, interpolatorInfo}
        sh.u32({ 0, ucodeLen, 0, 0, 0, uint32_t(interps.size()) << 5 });
        // VertexShader {field18, vertexElementCount, field20, elements+interps}
        sh.u32({ 0, uint32_t(declared.size()), 0 });
        for (auto& [i, vf] : declared)
        {
            const uint32_t usage = i == 0 ? POSITION : TEXCOORD;
            const uint32_t index = i == 0 ? 0 : uint32_t(i - 1);
            sh.u32({ vf->slot | (usage << 12) | (index << 16) });
        }
        for (uint32_t i : interps)
            sh.u32({ i | (TEXCOORD << 4) | (i << 8) });
    }
    else
    {
        std::vector<uint32_t> interps;
        for (uint32_t r : u.inputs)
            if (r < 16)
                interps.push_back(r);
        sh.u32({ 0, ucodeLen, 0, 31u << 8, 0, uint32_t(interps.size()) << 5 });
        // PixelShader {field18, outputs, interpolators}: color exports 0..3 map to
        // their bits, ucode export dst 61 = depth.
        uint32_t outputs = 0;
        for (uint32_t e : u.exports)
        {
            if (e < 4)
                outputs |= 1u << e;
            else if (e == 61)
                outputs |= 0x10;
        }
        sh.u32({ 0, outputs ? outputs : 1u });
        for (uint32_t r : interps)
            sh.u32({ r | (TEXCOORD << 4) | (r << 8) });
    }
    return sh.b;
}

static std::vector<uint8_t> BuildContainer(bool isVs, const uint8_t* ucode, size_t ucodeLen,
                                           const UcodeInfo& u,
                                           const std::vector<uint32_t>& tfSorted)
{
    const auto ctab = BuildCtab(isVs, tfSorted);
    const auto shader = BuildShaderStruct(isVs, uint32_t(ucodeLen), u);
    const uint32_t headerSize = 0x24;
    const uint32_t ctabOff = headerSize;
    const uint32_t shaderOff = ctabOff + uint32_t(ctab.size());
    const uint32_t virtualSize = shaderOff + uint32_t(shader.size());

    Blob out;
    // ShaderContainer {flags, virtualSize, physicalSize, fieldC, ctabOff,
    //                  defTableOff, shaderOff, field1C, field20}
    out.u32({ 0x102A1100u | (isVs ? 1u : 0u), virtualSize, uint32_t(ucodeLen), 0,
              ctabOff, 0, shaderOff, 0, 0 });
    out.raw(ctab.data(), ctab.size());
    out.raw(shader.data(), shader.size());
    assert(out.b.size() == virtualSize);
    out.raw(ucode, ucodeLen);
    return out.b;
}

// XenosRecomp USAGE_LOCATIONS: the Vulkan input location for original attribute
// enumeration index i (first vfetch = POSITION0, later = TEXCOORD i-1). Returns -2
// for an index outside the table — the C++ spelling of the Python KeyError, which
// fails the shader rather than guessing a location.
static int UsageLocation(int i)
{
    if (i == 0)
        return 0; // POSITION0
    const int t = i - 1;
    if (t < 4)
        return 4 + t;  // TEXCOORD0..3 -> 4..7
    if (t < 24)
        return 8 + t;  // TEXCOORD4..7 -> 12..15, 8..23 -> 16..31
    return -2;
}

// ---------------------------------------------------------------------------
// 4. ALU-const census — port of alu_const_census.py census_file(): every literal
// vc(N)/pc(N) read in the HLSL, and every dynamic (a0-relative) expression.

static void CensusHlsl(const std::string& hlsl, std::vector<int>& lits,
                       std::vector<std::string>& dyn)
{
    auto isWord = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    std::set<int> litSet;
    std::set<std::string> dynSet;
    size_t lineStart = 0;
    while (lineStart < hlsl.size())
    {
        size_t lineEnd = hlsl.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = hlsl.size();
        // Skip the macro's own definition, not a use.
        size_t f = lineStart;
        while (f < lineEnd && isspace((unsigned char)hlsl[f]))
            f++;
        if (hlsl.compare(f, 7, "#define") == 0)
        {
            lineStart = lineEnd + 1;
            continue;
        }
        for (size_t p = lineStart; p + 2 < lineEnd;)
        {
            if ((hlsl[p] == 'v' || hlsl[p] == 'p') && hlsl[p + 1] == 'c' &&
                hlsl[p + 2] == '(' && (p == lineStart || !isWord(hlsl[p - 1])))
            {
                const size_t q = hlsl.find(')', p + 3);
                if (q != std::string::npos && q < lineEnd && q > p + 3)
                {
                    std::string arg = hlsl.substr(p + 3, q - (p + 3));
                    // strip
                    size_t a = 0, b2 = arg.size();
                    while (a < b2 && isspace((unsigned char)arg[a])) a++;
                    while (b2 > a && isspace((unsigned char)arg[b2 - 1])) b2--;
                    arg = arg.substr(a, b2 - a);
                    const bool digitsOnly =
                        !arg.empty() &&
                        std::all_of(arg.begin(), arg.end(),
                                    [](char c) { return c >= '0' && c <= '9'; });
                    if (digitsOnly)
                        litSet.insert(atoi(arg.c_str()));
                    else if (arg != "INDEX")
                        dynSet.insert(arg);
                    p = q + 1;
                    continue;
                }
            }
            p++;
        }
        lineStart = lineEnd + 1;
    }
    lits.assign(litSet.begin(), litSet.end());
    dyn.assign(dynSet.begin(), dynSet.end());
}

// ---------------------------------------------------------------------------
// 5. sidecar JSON — a writer that reproduces Python json.dump(obj, indent=1) byte for
// byte, because the gate is a file diff and the runtime's sidecar reader was written
// against that exact shape. The two formatting facts that are easy to lose: an empty
// list renders as `[]` with no newline, and the item indent is depth chars (indent=1).

struct JVal
{
    enum Type { INT, BOOL, STR, ARR, OBJ } t = INT;
    long long num = 0;
    bool bv = false;
    std::string sv;
    std::vector<JVal> arr;
    // An OBJ's members, as parallel arrays rather than vector<pair<string,JVal>>:
    // a pair member instantiates against the still-incomplete JVal and fails to
    // compile, where a vector of the incomplete type itself is legal since C++17.
    std::vector<std::string> keys;

    static JVal I(long long v) { JVal j; j.t = INT; j.num = v; return j; }
    static JVal B(bool v) { JVal j; j.t = BOOL; j.bv = v; return j; }
    static JVal S(std::string v) { JVal j; j.t = STR; j.sv = std::move(v); return j; }
    static JVal A() { JVal j; j.t = ARR; return j; }
    static JVal O() { JVal j; j.t = OBJ; return j; }
    void add(const char* key, JVal v) // OBJ member append, insertion-ordered
    {
        keys.push_back(key);
        arr.push_back(std::move(v));
    }
};

static void JsonEscape(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20 || c >= 0x7F) // Python ensure_ascii; sidecar strings are ASCII
            {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += char(c);
        }
    }
    out += '"';
}

static void JsonWrite(std::string& out, const JVal& v, int depth)
{
    switch (v.t)
    {
    case JVal::INT: out += std::to_string(v.num); break;
    case JVal::BOOL: out += v.bv ? "true" : "false"; break;
    case JVal::STR: JsonEscape(out, v.sv); break;
    case JVal::ARR:
        if (v.arr.empty()) { out += "[]"; break; }
        out += "[\n";
        for (size_t i = 0; i < v.arr.size(); i++)
        {
            out.append(size_t(depth) + 1, ' ');
            JsonWrite(out, v.arr[i], depth + 1);
            if (i + 1 < v.arr.size())
                out += ',';
            out += '\n';
        }
        out.append(size_t(depth), ' ');
        out += ']';
        break;
    case JVal::OBJ:
        if (v.keys.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t i = 0; i < v.keys.size(); i++)
        {
            out.append(size_t(depth) + 1, ' ');
            JsonEscape(out, v.keys[i]);
            out += ": ";
            JsonWrite(out, v.arr[i], depth + 1);
            if (i + 1 < v.keys.size())
                out += ',';
            out += '\n';
        }
        out.append(size_t(depth), ' ');
        out += '}';
        break;
    }
}

// The whole sidecar: synthesize()'s meta plus alu_const_sidecar.py's annotate(),
// composed in one pass in the exact key order the two-stage Python pipeline produces.
static bool BuildMetaJson(bool isVs, const UcodeInfo& u,
                          const std::vector<uint32_t>& tfSorted,
                          const std::vector<int>& aluLits,
                          const std::vector<std::string>& aluDyn,
                          std::string& out, std::string& err)
{
    JVal meta = JVal::O();
    meta.add("kind", JVal::S(isVs ? "vs" : "ps"));
    JVal tf = JVal::A();
    for (uint32_t c : tfSorted)
        tf.arr.push_back(JVal::I(c));
    meta.add("tfetchConsts", std::move(tf));
    JVal td = JVal::A();
    for (uint32_t c : tfSorted) // positional against the sorted consts, by contract
        td.arr.push_back(JVal::I(u.tfetchDim[c]));
    meta.add("tfetchDims", std::move(td));

    if (isVs)
    {
        JVal attrs = JVal::A();
        for (size_t i = 0; i < u.vfetch.size(); i++)
        {
            const VFetch& vf = u.vfetch[i];
            int loc = -1;
            if (!vf.indirect)
            {
                loc = UsageLocation(int(i));
                if (loc == -2)
                {
                    err = "vertex attribute " + std::to_string(i) +
                          " has no input location (TEXCOORD>23)";
                    return false;
                }
            }
            JVal a = JVal::O();
            a.add("location", JVal::I(loc));
            a.add("bufferRead", JVal::I(vf.indirect));
            a.add("fetchSlot", JVal::I(vf.fetchSlot));
            a.add("format", JVal::I(vf.format));
            a.add("signed", JVal::I(vf.sgn));
            a.add("integer", JVal::I(vf.integer));
            a.add("strideDwords", JVal::I(vf.strideDwords));
            a.add("offsetDwords", JVal::I(vf.offsetDwords));
            a.add("srcReg", JVal::I(vf.srcReg));
            a.add("srcSwz", JVal::I(vf.srcSwz));
            a.add("indirect", JVal::I(vf.indirect));
            a.add("addrAttr", JVal::I(vf.addrAttr));
            attrs.arr.push_back(std::move(a));
        }
        meta.add("attributes", std::move(attrs));
    }

    JVal interp = JVal::A();
    if (isVs)
    {
        for (uint32_t e : u.exports)
            if (e < 16)
                interp.arr.push_back(JVal::I(e));
    }
    else
    {
        for (uint32_t r : u.inputs)
            if (r < 16)
                interp.arr.push_back(JVal::I(r));
    }
    meta.add("interpolators", std::move(interp));

    JVal alu = JVal::A();
    for (int r : aluLits)
        alu.arr.push_back(JVal::I(r));
    meta.add("aluConsts", std::move(alu));
    meta.add("aluDynamic", JVal::B(!aluDyn.empty()));
    if (!aluDyn.empty())
    {
        JVal d = JVal::A();
        for (auto& s : aluDyn)
            d.arr.push_back(JVal::S(s));
        meta.add("aluDynamicExprs", std::move(d));
    }

    out.clear();
    JsonWrite(out, meta, 0);
    out += '\n'; // alu_const_sidecar.py appends one after json.dump
    return true;
}

// ---------------------------------------------------------------------------
// 6. SPIR-V — DXC through its C API. dlopen'd rather than linked so a dev build
// without the library still builds and runs every non-renderer gate; the release
// bundles it in lib/ beside the executable. The argument list is
// tools/build_shader_spv.sh's, token for token — the probe that authorised this
// (part 84) showed the API and the CLI produce byte-identical SPIR-V for it.

static DxcCreateInstanceProc g_dxcCreate = nullptr;
static std::string g_dxcErr;
static std::once_flag g_dxcOnce;

static void LoadDxcOnce()
{
#ifdef _WIN32
    const char* libName = "dxcompiler.dll";
#elif defined(__APPLE__)
    const char* libName = "libdxcompiler.dylib";
#else
    const char* libName = "libdxcompiler.so";
#endif
    std::vector<std::string> candidates;
    if (const char* env = getenv("CZ_DXC_LIB"); env && *env)
        candidates.push_back(env);
    // The release layout: lib/ beside the executable (same place the bundled SDL2 and
    // ffmpeg live), then beside it directly.
    candidates.push_back((HostPaths::ExeDir() / "lib" / libName).string());
    candidates.push_back((HostPaths::ExeDir() / libName).string());
    // The dev layout: the sibling XenosRecomp checkout's dxc-bin.
    if (const char* home = getenv("HOME"); home && *home)
    {
        const std::string dxcbin = std::string(home) + "/GithubRepo/XenosRecomp/thirdparty/dxc-bin";
#ifdef __aarch64__
        const char* arch = "arm64";
#else
        const char* arch = "x64";
#endif
        candidates.push_back(dxcbin + "/lib/" + arch + "/" + libName);
        candidates.push_back(dxcbin + "/bin/" + arch + "/" + libName);
    }

    for (const auto& c : candidates)
    {
#ifdef _WIN32
        HMODULE h = LoadLibraryA(c.c_str());
        if (!h)
            continue;
        auto proc = reinterpret_cast<DxcCreateInstanceProc>(
            reinterpret_cast<void*>(GetProcAddress(h, "DxcCreateInstance")));
#else
        void* h = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h)
            continue;
        auto proc = reinterpret_cast<DxcCreateInstanceProc>(dlsym(h, "DxcCreateInstance"));
#endif
        if (proc)
        {
            g_dxcCreate = proc;
            fprintf(stderr, "[shxlate] dxcompiler: %s\n", c.c_str());
            return;
        }
    }
    g_dxcErr = "no dxcompiler library found (tried CZ_DXC_LIB, <exe>/lib, <exe>, "
               "the sibling XenosRecomp checkout)";
}

// One compiler instance per thread: DxcCreateInstance objects are not documented
// thread-safe across concurrent Compile calls, and upstream's own main.cpp uses
// exactly this thread_local pattern under par_unseq.
static IDxcCompiler3* ThreadCompiler(std::string& err)
{
    std::call_once(g_dxcOnce, LoadDxcOnce);
    if (!g_dxcCreate)
    {
        err = g_dxcErr;
        return nullptr;
    }
    thread_local IDxcCompiler3* comp = nullptr;
    if (!comp && FAILED(g_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&comp))))
    {
        err = "DxcCreateInstance failed";
        comp = nullptr;
    }
    return comp;
}

static std::wstring Widen(const std::string& s)
{
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s)
        w += wchar_t(c);
    return w;
}

static bool CompileSpirv(const std::string& hlsl, bool isVs, uint32_t tag,
                         std::vector<uint8_t>& spv, std::string& err)
{
    IDxcCompiler3* comp = ThreadCompiler(err);
    if (!comp)
        return false;

    std::vector<std::wstring> args = {
        L"-T", isVs ? L"vs_6_0" : L"ps_6_0",
        L"-HV", L"2021",
        L"-all-resources-bound",
        // No -fvk-invert-y, deliberately: the Xenos VS emits D3D-convention clip
        // coordinates and the renderer folds the window->NDC mapping itself
        // (build_shader_spv.sh carries the full note; Fable 2 paid a session for
        // the double flip).
        L"-spirv",
        L"-fvk-use-dx-layout",
        L"-Qstrip_debug",
        L"-D", Widen("XE_SHADER_TAG=" + std::to_string(tag)),
    };
    // CZ_DXC_DEFINES passthrough, same contract as the shell pipeline: extra
    // whitespace-separated tokens, so an arm cache can be built from the same
    // translator into a second directory.
    if (const char* extra = getenv("CZ_DXC_DEFINES"); extra && *extra)
    {
        std::string tok;
        for (const char* p = extra;; p++)
        {
            if (*p && !isspace((unsigned char)*p))
                tok += *p;
            else
            {
                if (!tok.empty())
                    args.push_back(Widen(tok));
                tok.clear();
                if (!*p)
                    break;
            }
        }
    }
    std::vector<const wchar_t*> argv;
    for (auto& a : args)
        argv.push_back(a.c_str());

    DxcBuffer src{};
    src.Ptr = hlsl.data();
    src.Size = hlsl.size();
    src.Encoding = DXC_CP_UTF8;

    IDxcResult* result = nullptr;
    if (FAILED(comp->Compile(&src, argv.data(), uint32_t(argv.size()), nullptr,
                             IID_PPV_ARGS(&result))) ||
        !result)
    {
        err = "dxc Compile() call failed";
        return false;
    }
    HRESULT status = HRESULT(0x80004005); // E_FAIL by value: win_compat.h #undefs the macro
    result->GetStatus(&status);
    if (FAILED(status))
    {
        err = "dxc rejected the HLSL";
        IDxcBlobUtf8* errors = nullptr;
        if (result->HasOutput(DXC_OUT_ERRORS) &&
            SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
            errors)
        {
            err += ": ";
            err.append(errors->GetStringPointer(),
                       std::min<size_t>(errors->GetStringLength(), 500));
            errors->Release();
        }
        result->Release();
        return false;
    }
    IDxcBlob* object = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    result->Release();
    if (!object)
    {
        err = "dxc produced no object";
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(object->GetBufferPointer());
    spv.assign(p, p + object->GetBufferSize());
    object->Release();
    return true;
}

// ---------------------------------------------------------------------------
// The pipeline, end to end.

static bool ParseName(const std::string& name, bool& isVs, uint32_t& tag,
                      std::string& err)
{
    isVs = name.rfind("vs_", 0) == 0; // anything else compiles as ps, like the shell
    const size_t us = name.find('_');
    if (us == std::string::npos || us + 1 >= name.size())
    {
        err = "shader name has no _<hash> suffix";
        return false;
    }
    char* end = nullptr;
    const unsigned long long h = strtoull(name.c_str() + us + 1, &end, 16);
    if (end == name.c_str() + us + 1)
    {
        err = "shader name suffix is not hex";
        return false;
    }
    tag = uint32_t(h & 0xFFFF);
    return true;
}
} // namespace

bool Translate(const std::string& name, const uint8_t* ucode, size_t size,
               Result& out, std::string& err)
{
    bool isVs = false;
    uint32_t tag = 0;
    if (!ParseName(name, isVs, tag, err))
        return false;

    UcodeInfo u;
    if (!ParseUcode(ucode, size, u, err))
        return false;

    std::vector<uint32_t> tfSorted = u.tfetchConsts;
    std::sort(tfSorted.begin(), tfSorted.end());

    const auto container = BuildContainer(isVs, ucode, size, u, tfSorted);

    // 3. HLSL — XenosRecomp's translator, on the fabricated container. A fresh
    // recompiler per call: the struct is cheap and shared state would be a race.
    ShaderRecompiler recompiler;
    recompiler.recompile(container.data(),
                         std::string_view(reinterpret_cast<const char*>(g_czShaderCommonH),
                                          g_czShaderCommonHSize));
    out.hlsl = std::move(recompiler.out);

    std::vector<int> aluLits;
    std::vector<std::string> aluDyn;
    CensusHlsl(out.hlsl, aluLits, aluDyn);

    if (!BuildMetaJson(isVs, u, tfSorted, aluLits, aluDyn, out.metaJson, err))
        return false;

    return CompileSpirv(out.hlsl, isVs, tag, out.spirv, err);
}

bool WritePair(const std::filesystem::path& outDir, const std::string& name,
               const Result& r)
{
    auto writeAll = [](const std::filesystem::path& p, const void* d, size_t len) {
        FILE* f = fopen(p.string().c_str(), "wb");
        if (!f)
            return false;
        const size_t w = fwrite(d, 1, len, f);
        fclose(f);
        return w == len;
    };
    const std::filesystem::path spv = outDir / (name + ".spv");
    const std::filesystem::path meta = outDir / (name + ".meta.json");
    if (writeAll(spv, r.spirv.data(), r.spirv.size()) &&
        writeAll(meta, r.metaJson.data(), r.metaJson.size()))
        return true;
    std::error_code ec;
    std::filesystem::remove(spv, ec); // never leave a .spv without its sidecar
    std::filesystem::remove(meta, ec);
    return false;
}

int TranslateDirectory(const char* ucodeDir, const char* outDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(ucodeDir, ec))
        if (e.path().extension() == ".ucode")
            files.push_back(e.path());
    if (ec)
    {
        fprintf(stderr, "[shxlate] cannot read %s: %s\n", ucodeDir, ec.message().c_str());
        return 1;
    }
    if (files.empty())
    {
        fprintf(stderr, "[shxlate] no *.ucode in %s\n", ucodeDir);
        return 1;
    }
    std::sort(files.begin(), files.end());
    fs::create_directories(outDir, ec);

    const char* keepHlsl = getenv("CZ_TRANSLATE_KEEP_HLSL");
    if (keepHlsl && *keepHlsl)
        fs::create_directories(keepHlsl, ec);

    std::atomic<size_t> next{ 0 };
    std::atomic<int> ok{ 0 };
    std::mutex failMx;
    std::vector<std::string> fails;

    auto worker = [&] {
        for (size_t i; (i = next.fetch_add(1)) < files.size();)
        {
            const std::string name = files[i].stem().string();
            std::string err;
            std::vector<uint8_t> bytes;
            {
                FILE* f = fopen(files[i].string().c_str(), "rb");
                if (!f)
                {
                    std::lock_guard<std::mutex> lk(failMx);
                    fails.push_back(name + " (unreadable)");
                    continue;
                }
                fseek(f, 0, SEEK_END);
                const long len = ftell(f);
                fseek(f, 0, SEEK_SET);
                bytes.resize(size_t(len));
                const size_t got = fread(bytes.data(), 1, bytes.size(), f);
                fclose(f);
                bytes.resize(got);
            }
            Result r;
            if (!Translate(name, bytes.data(), bytes.size(), r, err))
            {
                std::lock_guard<std::mutex> lk(failMx);
                fails.push_back(name + " (" + err + ")");
                continue;
            }
            if (!WritePair(outDir, name, r))
            {
                std::lock_guard<std::mutex> lk(failMx);
                fails.push_back(name + " (write failed)");
                continue;
            }
            if (keepHlsl && *keepHlsl)
            {
                std::ofstream hf(fs::path(keepHlsl) / (name + ".hlsl"), std::ios::binary);
                hf.write(r.hlsl.data(), std::streamsize(r.hlsl.size()));
            }
            ok.fetch_add(1);
        }
    };

    // Whole-machine parallel on purpose: this is an offline batch mode (the gate, and
    // D.3's first-run pass), not something that runs beside the game.
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const size_t nThreads = std::min<size_t>(hw, files.size());
    std::vector<std::thread> pool;
    for (size_t t = 1; t < nThreads; t++)
        pool.emplace_back(worker);
    worker();
    for (auto& t : pool)
        t.join();

    for (auto& f : fails)
        fprintf(stderr, "[shxlate] FAILED: %s\n", f.c_str());
    fprintf(stderr, "[shxlate] translated %d of %zu shaders into %s\n",
            ok.load(), files.size(), outDir);
    return fails.empty() ? 0 : 1;
}
} // namespace ShaderTranslator
