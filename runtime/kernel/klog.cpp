#include "klog.h"

#include <cstdlib>
#include <cstring>

#include "../cpu/crash_report.h"

namespace {

// CZ_KCALL_WHO=Name1,Name2 — dump the guest call stack the first time each of these
// imports is called.
//
// WHY: the phase gate says *that* our first-occurrence order diverges from A1's, and
// the most useful divergences are the ones where we call something hardware never
// calls at all. Case Zero's gate position 57 is `RtlCompareStringN`, which appears
// ZERO times in A1 — so the interesting question is not "what does it return" but
// "which guest function asked, and why is it on a path hardware never enters".
// Nothing else answers that: the name is the only thing the trace records, and by
// the time the divergence is visible in a diff the call is long gone.
//
// Substring-free exact match on the comma-separated list, so `NetDll_select` cannot
// be triggered by asking for `NetDll_selectX` or vice versa.
bool WantsBacktrace(const char* name)
{
    const char* list = getenv("CZ_KCALL_WHO");
    if (!list)
        return false;
    const size_t n = std::strlen(name);
    for (const char* p = list; *p;)
    {
        const char* comma = std::strchr(p, ',');
        const size_t len = comma ? size_t(comma - p) : std::strlen(p);
        if (len == n && std::strncmp(p, name, n) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

} // namespace

void KernelCallTrace(const char* name, uint64_t hit)
{
    // The recompiler names imports `__imp__<Name>`; Xenia logs `<Name>`. Strip so
    // the two sides of the gate diff are directly comparable without the tool
    // having to know about the recompiler's naming at all.
    if (std::strncmp(name, "__imp__", 7) == 0)
        name += 7;

    if (hit == 0)
        std::fprintf(stderr, "[kcall] %s\n", name);
    else
        std::fprintf(stderr, "[kcall+] %s hit %llu times\n", name,
                     static_cast<unsigned long long>(hit));

    if (hit == 0 && WantsBacktrace(name))
        CzDumpGuestBacktrace(name);
}
