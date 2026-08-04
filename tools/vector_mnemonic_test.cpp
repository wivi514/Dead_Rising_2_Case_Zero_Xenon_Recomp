// Differential test for the vector mnemonics added to XenonRecomp's recompiler
// for Dead Rising 2: Case Zero — vadduhs, vadduws, vsubuws, vpkuwum, vspltish.
//
// Why this exists: three of those five are not one-to-one with an x86 intrinsic.
// vadduws/vsubuws have no 32-bit unsigned saturating counterpart in any SSE
// level, so the emitted code uses algebraic identities (a + min_epu32(b,~a) and
// max_epu32(a,b) - b) that are easy to get subtly wrong; vpkuwum has to swap its
// operands because XenonRecomp stores vectors fully byte-reversed. Eyeballing
// the emitted line proves nothing about any of that.
//
// Method: model a PPC vector in PPC element order, convert to XenonRecomp's
// reversed host layout, run the EXACT intrinsic sequence the recompiler emits,
// convert back, and compare against a scalar reference written straight from the
// PPC semantics. Random + edge-case vectors.
//
// Build and run:
//     g++ -O2 -msse4.1 -I ~/GithubRepo/XenonRecomp/thirdparty \
//         -o /tmp/vt tools/vector_mnemonic_test.cpp && /tmp/vt
//
// Run it under -O2, -msse4.1, -mavx2 AND -O0: simde dispatches to different
// implementations per target, so passing on one says nothing about the others.
//
// If you add a case here, break it on purpose once and confirm the test screams.
// A vector test that has never failed has not been shown capable of failing, and
// both failure modes here (wrong pack operand order, wrong saturation edge) are
// silent wrong *values* that no amount of reading the emitted line will reveal.
// For the record, the controls used when this was written: unswapping vpkuwum's
// operands fails 199,957 of 200,153 cases, and dropping vsubuws's clamp fails
// 184,854.

#include <simde/x86/sse4.1.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

// XenonRecomp's guest vector register: 16 bytes, addressed as u8/u16/u32.
union VReg {
    uint8_t  u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
};

// PPC-order vector (element 0 is the leftmost/most significant) -> host layout.
// XenonRecomp reverses the whole 16-byte vector, so PPC byte i lands at host
// byte 15-i, which is what makes element indices flip and pack operands swap.
static VReg toHost(const uint8_t ppc[16]) {
    VReg v;
    for (int i = 0; i < 16; i++) v.u8[i] = ppc[15 - i];
    return v;
}
static void fromHost(const VReg& v, uint8_t ppc[16]) {
    for (int i = 0; i < 16; i++) ppc[15 - i] = v.u8[i];
}

// Read/write PPC element n from a PPC-order byte array (big-endian elements).
static uint32_t ppcW(const uint8_t p[16], int n) {
    return (uint32_t(p[n*4]) << 24) | (uint32_t(p[n*4+1]) << 16) |
           (uint32_t(p[n*4+2]) << 8) | uint32_t(p[n*4+3]);
}
static uint16_t ppcH(const uint8_t p[16], int n) {
    return uint16_t((uint32_t(p[n*2]) << 8) | p[n*2+1]);
}
static void setPpcW(uint8_t p[16], int n, uint32_t x) {
    p[n*4] = uint8_t(x >> 24); p[n*4+1] = uint8_t(x >> 16);
    p[n*4+2] = uint8_t(x >> 8); p[n*4+3] = uint8_t(x);
}
static void setPpcH(uint8_t p[16], int n, uint16_t x) {
    p[n*2] = uint8_t(x >> 8); p[n*2+1] = uint8_t(x);
}

// ---------------------------------------------------------------------------
// The emitted sequences, copied verbatim from recompiler.cpp.
// ---------------------------------------------------------------------------
static void emit_vadduhs(VReg& d, const VReg& a, const VReg& b) {
    simde_mm_store_si128((simde__m128i*)d.u16, simde_mm_adds_epu16(simde_mm_load_si128((simde__m128i*)a.u16), simde_mm_load_si128((simde__m128i*)b.u16)));
}
static void emit_vadduws(VReg& d, const VReg& a, const VReg& b) {
    simde_mm_store_si128((simde__m128i*)d.u32, simde_mm_add_epi32(simde_mm_load_si128((simde__m128i*)a.u32), simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*)b.u32), simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*)a.u32), simde_mm_set1_epi32(-1)))));
}
static void emit_vsubuws(VReg& d, const VReg& a, const VReg& b) {
    simde_mm_store_si128((simde__m128i*)d.u32, simde_mm_sub_epi32(simde_mm_max_epu32(simde_mm_load_si128((simde__m128i*)a.u32), simde_mm_load_si128((simde__m128i*)b.u32)), simde_mm_load_si128((simde__m128i*)b.u32)));
}
// NOTE: the recompiler emits vB as the first packus argument and vA as the
// second; this helper takes (a,b) in PPC order and applies that swap itself.
static void emit_vpkuwum(VReg& d, const VReg& a, const VReg& b) {
    simde_mm_store_si128((simde__m128i*)d.u16, simde_mm_packus_epi32(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)b.u32), simde_mm_set1_epi32(0xFFFF)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)a.u32), simde_mm_set1_epi32(0xFFFF))));
}
static void emit_vspltish(VReg& d, int16_t simm) {
    simde_mm_store_si128((simde__m128i*)d.u16, simde_mm_set1_epi16(simm));
}

// ---------------------------------------------------------------------------
// Scalar references, straight from the PowerPC/VMX definitions.
// ---------------------------------------------------------------------------
static void ref_vadduhs(uint8_t d[16], const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 8; i++) {
        uint32_t s = uint32_t(ppcH(a, i)) + uint32_t(ppcH(b, i));
        setPpcH(d, i, uint16_t(s > 0xFFFF ? 0xFFFF : s));
    }
}
static void ref_vadduws(uint8_t d[16], const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 4; i++) {
        uint64_t s = uint64_t(ppcW(a, i)) + uint64_t(ppcW(b, i));
        setPpcW(d, i, uint32_t(s > 0xFFFFFFFFull ? 0xFFFFFFFFull : s));
    }
}
static void ref_vsubuws(uint8_t d[16], const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 4; i++) {
        uint32_t x = ppcW(a, i), y = ppcW(b, i);
        setPpcW(d, i, x > y ? x - y : 0);   // saturate at zero
    }
}
static void ref_vpkuwum(uint8_t d[16], const uint8_t a[16], const uint8_t b[16]) {
    // Low halfword of each word: vA's four words -> result halfwords 0..3,
    // vB's four words -> result halfwords 4..7. No saturation.
    for (int i = 0; i < 4; i++) setPpcH(d, i,     uint16_t(ppcW(a, i) & 0xFFFF));
    for (int i = 0; i < 4; i++) setPpcH(d, i + 4, uint16_t(ppcW(b, i) & 0xFFFF));
}
static void ref_vspltish(uint8_t d[16], int16_t simm) {
    for (int i = 0; i < 8; i++) setPpcH(d, i, uint16_t(simm));
}

// ---------------------------------------------------------------------------
static int failures = 0;
static void check(const char* name, const uint8_t got[16], const uint8_t want[16],
                  const uint8_t a[16], const uint8_t b[16]) {
    if (memcmp(got, want, 16) == 0) return;
    if (++failures > 5) return;
    printf("  FAIL %s\n    a   =", name);
    for (int i = 0; i < 16; i++) printf(" %02X", a[i]);
    printf("\n    b   =");
    for (int i = 0; i < 16; i++) printf(" %02X", b[i]);
    printf("\n    got =");
    for (int i = 0; i < 16; i++) printf(" %02X", got[i]);
    printf("\n    want=");
    for (int i = 0; i < 16; i++) printf(" %02X", want[i]);
    printf("\n");
}

int main() {
    std::mt19937 rng(12345);
    // Edge words chosen to straddle every saturation boundary.
    const uint32_t edges[] = {0, 1, 2, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
                              0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF};
    const int nEdges = int(sizeof(edges) / sizeof(edges[0]));

    uint8_t a[16], b[16], want[16], got[16];
    VReg ha, hb, hd;

    long cases = 0;

    auto runOne = [&]() {
        ha = toHost(a); hb = toHost(b);
        emit_vadduhs(hd, ha, hb); fromHost(hd, got); ref_vadduhs(want, a, b);
        check("vadduhs", got, want, a, b);
        emit_vadduws(hd, ha, hb); fromHost(hd, got); ref_vadduws(want, a, b);
        check("vadduws", got, want, a, b);
        emit_vsubuws(hd, ha, hb); fromHost(hd, got); ref_vsubuws(want, a, b);
        check("vsubuws", got, want, a, b);
        emit_vpkuwum(hd, ha, hb); fromHost(hd, got); ref_vpkuwum(want, a, b);
        check("vpkuwum", got, want, a, b);
        cases++;
    };

    // Exhaustive over edge-word pairs, broadcast to all four lanes.
    for (int i = 0; i < nEdges; i++)
        for (int j = 0; j < nEdges; j++) {
            for (int k = 0; k < 4; k++) { setPpcW(a, k, edges[i]); setPpcW(b, k, edges[j]); }
            runOne();
        }

    // Random vectors, plus random mixtures of edge values per lane.
    for (int n = 0; n < 200000; n++) {
        for (int k = 0; k < 4; k++) {
            setPpcW(a, k, (n & 1) ? edges[rng() % nEdges] : uint32_t(rng()));
            setPpcW(b, k, (n & 1) ? edges[rng() % nEdges] : uint32_t(rng()));
        }
        runOne();
    }

    // vspltish over the whole 5-bit signed immediate range.
    for (int s = -16; s <= 15; s++) {
        emit_vspltish(hd, int16_t(s)); fromHost(hd, got);
        ref_vspltish(want, int16_t(s));
        uint8_t zero[16] = {};
        check("vspltish", got, want, zero, zero);
        cases++;
    }

    printf("%ld cases, %d failures\n", cases, failures);
    return failures != 0;
}
