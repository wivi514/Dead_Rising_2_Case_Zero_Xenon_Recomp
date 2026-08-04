#include "xex_imports.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <xbox.h>
#include <xex.h>

#include "heap.h"
#include "klog.h"
#include "memory.h"

std::atomic<uint32_t> g_keTimeStampBundle{ 0 };
std::atomic<uint32_t> g_xexHeaderBase{ 0 };

// The xboxkrnl variable exports Case Zero actually imports.
//
// Not a general table copied from a template port — it is exactly the 13 unpaired
// type-0 descriptors in this image's import table, and it was verified against A1's
// own import dump, which marks variable imports with a `V` and prints the ordinal:
//
//   V 82000578  156 (342)  XboxHardwareInfo          V 82000640  158 (344)  XboxKrnlVersion
//   V 8200059C  1AE (430)  ExLoadedCommandLine       V 82000758  1BE (446)  VdGlobalDevice
//   V 820005A0  059 ( 89)  KeDebugMonitorData        V 8200075C  1BF (447)  VdGlobalXamDevice
//   V 820005AC  01B ( 27)  ExThreadObjectType        V 82000764  266 (614)  KeCertMonitorData
//   V 820005F4  00E ( 14)  ExEventObjectType         V 820007C8  1C0 (448)  VdGpuClockInMHz
//   V 82000600  193 (403)  XexExecutableModuleHandle V 820007D8  1C1 (449)  VdHSIOCalibrationLock
//   V 8200062C  0AD (173)  KeTimeStampBundle
//
// Worth recording for the Case West port, because it is a stronger result than
// "the code transferred": Case Zero's variable set is IDENTICAL to Asura's Wrath's
// — same 13 ordinals — despite a different studio, engine and SDK year. These are
// what the XDK's static libraries pull in, so the set is a property of the SDK
// rather than of the title. Do not take that as licence to skip the check: the
// same A1 dump also confirms the 244 `F` entries match `ppc_recomp_shared.h`
// exactly, and the check costs one grep.
//
// Anything not listed still gets 16 zeroed bytes plus a log line, so a future
// import-set change is visible rather than silent.
namespace
{
struct KernelVariable
{
    const char* name;
    uint32_t size;
};

const KernelVariable* LookupKernelVariable(uint32_t ordinal)
{
    static const struct { uint32_t ordinal; KernelVariable var; } kVars[] = {
        { 0x00E, { "ExEventObjectType",         0x20 } }, // OBJECT_TYPE; only its address is used
        { 0x01B, { "ExThreadObjectType",        0x20 } },
        { 0x059, { "KeDebugMonitorData",        4    } },
        { 0x0AD, { "KeTimeStampBundle",         0x18 } }, // {interrupt time, system time, tick count}
        { 0x156, { "XboxHardwareInfo",          0x10 } },
        { 0x158, { "XboxKrnlVersion",           8    } },
        { 0x193, { "XexExecutableModuleHandle", 4    } },
        { 0x1AE, { "ExLoadedCommandLine",       1024 } },
        { 0x1BE, { "VdGlobalDevice",            4    } }, // D3D stores its device pointer here
        { 0x1BF, { "VdGlobalXamDevice",         4    } },
        { 0x1C0, { "VdGpuClockInMHz",           4    } },
        { 0x1C1, { "VdHSIOCalibrationLock",     28   } }, // RTL_CRITICAL_SECTION
        { 0x266, { "KeCertMonitorData",         4    } },
    };
    for (const auto& v : kVars)
        if (v.ordinal == ordinal)
            return &v.var;
    return nullptr;
}

uint32_t AllocateVariable(uint32_t ordinal, const char* libName)
{
    const KernelVariable* known = LookupKernelVariable(ordinal);
    const uint32_t size = known ? known->size : 16;
    auto* host = static_cast<uint8_t*>(g_heap.Alloc(size));
    if (!host)
        return 0;
    memset(host, 0, size);
    const uint32_t guest = g_memory.MapVirtual(host);

    switch (ordinal)
    {
    case 0x0AD: // KeTimeStampBundle: kept current by the vsync pump from phase 3 on
        g_keTimeStampBundle = guest;
        break;
    case 0x156: // XboxHardwareInfo: {flags, cpu count, ...} — 3 cores x 2 threads
        host[4] = 6;
        break;
    case 0x158: // XboxKrnlVersion — and Case Zero DOES branch on it, so this stopped
                // being a free constant (the note that used to sit here said that
                // would be a finding; it is finding 31).
                //
                // sub_825D7AC8, the rumble path, reads this struct and takes a
                // legacy code path only when major == 2, minor == 0 and build <
                // 5611. The capture's config line is `kernel_build_version = 1888`,
                // so A1 takes it; the 2.0.14448.0 both template ports report does
                // not. Matching the capture is the whole basis of the phase gate, so
                // 1888 it is — and it is also the conservative direction, because a
                // version is a claim about which XAM entry points exist and ours is
                // a minimal XAM (gotcha 58: raise a version gate only together with
                // the exports it unlocks).
                //
                // MEASURED, so the claim is not oversold: three 25 s runs at each
                // value reach 82/85/82 and 85/82/85 visible kernel calls — the same
                // distribution, and the 82-vs-85 spread is boot timing, not the
                // version (gotcha 50: one arm is not a measurement). So this is
                // chosen for faithfulness to the capture's control flow, NOT because
                // it was observed to get the boot further.
        *reinterpret_cast<be<uint16_t>*>(host + 0) = 2;
        *reinterpret_cast<be<uint16_t>*>(host + 2) = 0;
        *reinterpret_cast<be<uint16_t>*>(host + 4) = 1888;
        break;
    case 0x193: // XexExecutableModuleHandle: the guest-resident XEX header block
        *reinterpret_cast<be<uint32_t>*>(host) = g_xexHeaderBase.load();
        break;
    case 0x1AE: // ExLoadedCommandLine
        strcpy(reinterpret_cast<char*>(host), "default.xex");
        break;
    case 0x1C0: // VdGpuClockInMHz
        *reinterpret_cast<be<uint32_t>*>(host) = 500;
        break;
    default:
        break;
    }

    fprintf(stderr, "[loader] data import %s!%s (ord 0x%X) -> 0x%08X (%u bytes)\n", libName,
            known ? known->name : "<UNKNOWN — check the ordinal against xboxkrnl_table.inc>",
            ordinal, guest, size);
    return guest;
}
} // namespace

uint32_t PublishXexHeaders(const uint8_t* xexFile, size_t xexFileSize)
{
    // Xex2Header: magic, moduleFlags, sizeOfHeaders, ... — every optional-header
    // offset is relative to the module start, so copying the first sizeOfHeaders
    // bytes verbatim gives a self-consistent block the guest can walk.
    const uint32_t sizeOfHeaders =
        __builtin_bswap32(*reinterpret_cast<const uint32_t*>(xexFile + 8));
    const uint32_t copy =
        std::min<uint32_t>(sizeOfHeaders, static_cast<uint32_t>(xexFileSize));

    auto* host = static_cast<uint8_t*>(g_heap.Alloc(copy));
    if (!host)
        return 0;
    memcpy(host, xexFile, copy);
    const uint32_t guest = g_memory.MapVirtual(host);
    g_xexHeaderBase = guest;
    fprintf(stderr, "[loader] XEX headers published at %08X (%u bytes)\n", guest, copy);
    return guest;
}

void ResolveXexDataImports(const uint8_t* xexFile)
{
    const auto* importHeader = reinterpret_cast<const Xex2ImportHeader*>(
        getOptHeaderPtr(xexFile, XEX_HEADER_IMPORT_LIBRARIES));
    if (!importHeader)
    {
        KLOG("XEX declares no import libraries — nothing to resolve\n");
        return;
    }

    // Library name string table (each name padded to 4 bytes).
    const char* strTable = reinterpret_cast<const char*>(importHeader + 1);
    const uint32_t numNames = importHeader->numImports;
    const char* names[32] = {};
    {
        size_t offset = 0;
        for (uint32_t i = 0; i < numNames && i < 32; i++)
        {
            names[i] = strTable + offset;
            offset += ((strlen(strTable + offset) + 1) + 3) & ~3ull;
        }
    }

    const auto* library = reinterpret_cast<const Xex2ImportLibrary*>(
        reinterpret_cast<const char*>(importHeader + 1) + importHeader->sizeOfStringTable);

    uint32_t functions = 0, variables = 0;
    for (uint32_t lib = 0; lib < numNames; lib++)
    {
        const char* libName =
            library->name < numNames && names[library->name] ? names[library->name] : "?";
        const auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
        const uint16_t count = library->numberOfImports;

        for (uint16_t i = 0; i < count; i++)
        {
            const uint32_t slotVA = descriptors[i].firstThunk;

            // Descriptors come in two flavours and the VA alone separates them
            // cleanly: call thunks live in the code section (>= PPC_CODE_BASE),
            // IAT slots live in the read-only data below it. On this image A1's
            // dump puts every IAT slot in 0x82000400..0x820007DC (inside `.rdata`)
            // and every thunk in 0x829C2xxx..0x829C3xxx (inside `.text`).
            //
            // (Fable 2 discriminated by looking for the nop/nop/nop/blr word the
            // loader writes over a thunk. That also works, but it couples this code
            // to the loader's rewrite — and gotcha 22 is precisely about a scan
            // that depended on which stage of loading the image was in. The address
            // ranges are a property of the image itself.)
            if (slotVA >= PPC_CODE_BASE)
                continue;

            const bool pairedWithThunk =
                (i + 1 < count) && descriptors[i + 1].firstThunk >= PPC_CODE_BASE;

            if (pairedWithThunk)
            {
                // Function import: point the IAT slot at the call thunk, so an
                // indirect call through the slot reaches the recompiled dispatcher
                // (the thunk VA is in PPCFuncMappings, bound to __imp__<Name>).
                *reinterpret_cast<be<uint32_t>*>(g_memory.Translate(slotVA)) =
                    descriptors[i + 1].firstThunk;
                ++functions;
                continue;
            }

            // No thunk follows: a kernel-exported *variable*. The slot must hold the
            // address of real storage — the guest dereferences it.
            const uint32_t thunkValue =
                *reinterpret_cast<const uint32_t*>(g_memory.Translate(slotVA));
            // Xex2LoadImage byte-swapped the type-0 words in place, so guest memory
            // holds them host-endian: ordinal in the low 16 bits.
            *reinterpret_cast<be<uint32_t>*>(g_memory.Translate(slotVA)) =
                AllocateVariable(thunkValue & 0xFFFF, libName);
            ++variables;
        }

        library = reinterpret_cast<const Xex2ImportLibrary*>(
            reinterpret_cast<const char*>(library + 1) + count * sizeof(Xex2ImportDescriptor));
    }
    fprintf(stderr, "[loader] resolved %u function IAT slots + %u kernel variables\n",
            functions, variables);
    // A1's import dump is the cross-check: 244 `F` entries and 13 `V` entries, and
    // `ppc_recomp_shared.h` declares exactly 244 `__imp__` externs. A count that
    // disagrees means the descriptor walk drifted, not that the title changed.
    if (variables != 13 || functions != 244)
        fprintf(stderr,
                "[loader] WARNING: expected 244 function slots + 13 variables from this "
                "image (A1's import dump and ppc_recomp_shared.h agree on those "
                "numbers) — got %u and %u\n",
                functions, variables);
}
