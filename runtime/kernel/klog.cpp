#include "klog.h"

#include <cstring>

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
}
