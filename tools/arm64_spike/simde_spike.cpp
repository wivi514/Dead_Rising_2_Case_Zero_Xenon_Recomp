// The macOS/ARM64 spike, reduced to the question the plan turns on: does SIMDe lower the
// VMX lowerings ppc_context.h actually uses to NEON, or to a scalar loop?
// The ops picked are the ones runtime/CMakeLists.txt names as "not optional": the packs
// and the min/max_epu32 used by the saturating vector arithmetic.
#include <x86/sse4.1.h>
#include <x86/avx.h>

extern "C" {
simde__m128i t_min_epu32(simde__m128i a, simde__m128i b){ return simde_mm_min_epu32(a,b); }
simde__m128i t_max_epu32(simde__m128i a, simde__m128i b){ return simde_mm_max_epu32(a,b); }
simde__m128i t_packs_16 (simde__m128i a, simde__m128i b){ return simde_mm_packs_epi16(a,b); }
simde__m128i t_packus_16(simde__m128i a, simde__m128i b){ return simde_mm_packus_epi16(a,b); }
simde__m128i t_packs_32 (simde__m128i a, simde__m128i b){ return simde_mm_packs_epi32(a,b); }
simde__m128  t_blendv   (simde__m128 a, simde__m128 b, simde__m128 m){ return simde_mm_blendv_ps(a,b,m); }
simde__m128i t_shuf8    (simde__m128i a, simde__m128i b){ return simde_mm_shuffle_epi8(a,b); }
simde__m128  t_dp       (simde__m128 a, simde__m128 b){ return simde_mm_dp_ps(a,b,0xFF); }
simde__m128i t_cvtps    (simde__m128 a){ return simde_mm_cvtps_epi32(a); }
int          t_movemask (simde__m128 a){ return simde_mm_movemask_ps(a); }
}
