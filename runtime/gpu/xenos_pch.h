// The pch XenosRecomp's shader_recompiler.cpp is compiled against inside THIS tree.
//
// WHY THIS FILE EXISTS
// --------------------
// Release D.2 links XenosRecomp's translator (MIT, sibling checkout) directly into the
// runtime so a shipped build can turn microcode into SPIR-V with no Python, no shell and
// no XenosRecomp executable on the player's machine. That translator is compiled upstream
// with `target_precompile_headers(XenosRecomp PRIVATE pch.h)`, so its sources assume a
// header they never #include: `be<T>`, `byteSwap`, fmt, and a handful of std headers.
//
// We cannot just force-include upstream's pch.h. It pulls in smol-v, zstd, xxhash and
// dxcapi — the first three are libraries this runtime does not link and does not need
// (we want ONE shader's HLSL, not the compressed multi-shader cache blob main.cpp
// builds), and dxcapi's WinAdapter needs -fms-extensions, a flag the recompiler TU
// itself has no use for (only shader_translator.cpp talks to DXC, and it includes
// dxcapi itself). So this is upstream's pch.h minus those four, and it must stay a
// SUPERSET of what shader_recompiler.cpp actually uses or the build breaks loudly at
// compile time, which is the failure mode we want.
//
// It is deliberately a copy rather than an include of upstream's file: upstream's pch is
// its build's private business and adding our constraint to it would make the sibling
// checkout carry a patch for us. This project already carries local XenosRecomp patches
// (docs/xenonrecomp-upstream-bugs.md) and every one of them is a cost at the next rebase.
#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <fmt/core.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

template<typename T>
static T byteSwap(T value)
{
    if constexpr (sizeof(T) == 1)
        return value;
    else if constexpr (sizeof(T) == 2)
        return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(value)));
    else if constexpr (sizeof(T) == 4)
        return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(value)));
    else if constexpr (sizeof(T) == 8)
        return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(value)));

    assert(false && "Unexpected byte size.");
    return value;
}

template<typename T>
struct be
{
    T value;

    T get() const
    {
        if constexpr (std::is_enum_v<T>)
            return T(byteSwap(std::underlying_type_t<T>(value)));
        else
            return byteSwap(value);
    }

    operator T() const
    {
        return get();
    }
};
