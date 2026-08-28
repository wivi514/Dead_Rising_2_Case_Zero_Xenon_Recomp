#include "thread_budget.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace
{

const char* Env(const char* n)
{
    const char* v = getenv(n);
    return (v && *v) ? v : nullptr;
}

// Read a small sysfs integer. Returns false if the file is absent or unreadable, which
// is the normal case inside a container with a trimmed /sys and must not be fatal.
bool ReadInt(const std::string& path, int* out)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    int v = 0;
    const bool ok = fscanf(f, "%d", &v) == 1;
    fclose(f);
    if (ok)
        *out = v;
    return ok;
}

// How many PHYSICAL cores this process may actually run on.
//
// Three things are deliberate here. It counts unique (package, core) pairs rather than
// dividing the logical count by an assumed threads-per-core, because heterogeneous parts
// exist — Intel's P/E split has SMT on some cores and not others, so any single divisor
// is wrong on those machines. It intersects with the process's CPU AFFINITY MASK, so a
// run under `taskset` or in a cpuset-constrained container budgets against what it was
// given rather than against what the silicon has. And when the topology cannot be read at
// all it returns 0 so the caller can say which fallback it took, rather than quietly
// inventing a plausible number.
#if defined(_WIN32)
// Windows topology. The POSIX path below reads /sys, which does not exist here, so
// without this the count falls to the "assume SMT, halve the logical count" fallback —
// which on the 12700H in the build laptop reported 10 physical cores against a real 14
// (6 performance + 8 efficiency) and sized the worker budget off the wrong number.
//
// GetLogicalProcessorInformationEx(RelationProcessorCore) returns one record PER
// PHYSICAL CORE, which is the question being asked; counting records is the answer, and
// it is correct on hybrid P/E parts where halving the thread count is not.
unsigned CountPhysicalCoresWin()
{
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (!bytes)
        return 0;
    std::vector<uint8_t> buf(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes))
        return 0;
    unsigned cores = 0;
    for (DWORD off = 0; off < bytes;)
    {
        auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (rec->Size == 0)
            break;
        if (rec->Relationship == RelationProcessorCore)
            ++cores;
        off += rec->Size;
    }
    return cores;
}
#endif

unsigned CountPhysicalCores()
{
#if defined(__linux__)
    cpu_set_t* set = nullptr;
    size_t setSize = 0;
    unsigned allowedMax = 0;
    // The mask can be larger than CPU_SETSIZE on big machines, so grow until it fits.
    for (unsigned n = 1024; n <= 65536; n *= 2)
    {
        set = CPU_ALLOC(n);
        if (!set)
            break;
        setSize = CPU_ALLOC_SIZE(n);
        if (sched_getaffinity(0, setSize, set) == 0)
        {
            allowedMax = n;
            break;
        }
        CPU_FREE(set);
        set = nullptr;
    }

    std::set<std::pair<int, int>> cores;
    unsigned allowedCpus = 0;
    for (unsigned cpu = 0; cpu < allowedMax; ++cpu)
    {
        if (!CPU_ISSET_S(cpu, setSize, set))
            continue;
        ++allowedCpus;
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        int pkg = 0, core = 0;
        if (ReadInt(base + "core_id", &core))
        {
            if (!ReadInt(base + "physical_package_id", &pkg))
                pkg = 0;
            cores.insert({ pkg, core });
        }
        else
        {
            // No topology for a CPU we are allowed to use: treat it as its own core, so
            // the count degrades towards the logical count instead of towards zero.
            cores.insert({ -1, int(cpu) });
        }
    }
    if (set)
        CPU_FREE(set);
    if (!cores.empty())
        return unsigned(cores.size());
    if (allowedCpus)
        return allowedCpus;
#endif
    return 0;
}

struct Grant
{
    std::string pool;
    unsigned granted = 0;
    unsigned desired = 0;
    const char* how = "budget";   // "budget", "env", or "starved"
};

struct BudgetState
{
    unsigned physical = 0;
    unsigned logical = 0;
    unsigned total = 0;
    unsigned left = 0;
    bool overridden = false;      // CZ_WORKERS set
    bool topologyOk = true;
    std::vector<Grant> grants;
    bool dirty = true;
};

BudgetState& State()
{
    static BudgetState s = [] {
        BudgetState st;
        st.logical = std::thread::hardware_concurrency();
        if (!st.logical)
            st.logical = 1;
#if defined(_WIN32)
        st.physical = CountPhysicalCoresWin();
#else
        st.physical = CountPhysicalCores();
#endif
        if (!st.physical)
        {
            // No topology at all. Assume SMT rather than not: over-counting cores would
            // hand out workers this machine does not have, and a budget that errs must
            // err towards leaving the user's machine usable.
            st.topologyOk = false;
            st.physical = st.logical > 1 ? st.logical / 2 : 1;
        }

        // THE POLICY. See the header for the derivation and the sanity table.
        const unsigned reserved = 2;    // OS/compositor + the user's own software
        const unsigned committed = 3;   // the graphics pump + the two busy guest threads
        const unsigned cap = 6;         // §0's ceiling; the PM4 walk is serial
        unsigned budget = 0;
        if (st.physical > reserved + committed)
            budget = st.physical - reserved - committed;
        if (budget > cap)
            budget = cap;

        if (const char* s = Env("CZ_WORKERS"))
        {
            budget = unsigned(strtoul(s, nullptr, 10));
            st.overridden = true;
        }
        st.total = budget;
        st.left = budget;
        return st;
    }();
    return s;
}

}   // namespace

unsigned ThreadBudget_PhysicalCores() { return State().physical; }
unsigned ThreadBudget_LogicalCpus() { return State().logical; }
unsigned ThreadBudget_Total() { return State().total; }

unsigned ThreadBudget_Take(const char* pool, unsigned desired, const char* overrideEnv)
{
    BudgetState& s = State();
    const char* name = pool ? pool : "?";
    for (const Grant& g : s.grants)
        if (g.pool == name)
            return g.granted;   // idempotent; see the header

    Grant g;
    g.pool = name;
    g.desired = desired;
    if (overrideEnv)
    {
        if (const char* v = Env(overrideEnv))
        {
            g.granted = unsigned(strtoul(v, nullptr, 10));
            g.how = "env";
            s.grants.push_back(g);
            s.dirty = true;
            return g.granted;
        }
    }
    g.granted = desired < s.left ? desired : s.left;
    if (g.granted < desired)
        g.how = s.left ? "clamped" : "starved";
    s.left -= g.granted;
    s.grants.push_back(g);
    s.dirty = true;
    return g.granted;
}

void ThreadBudget_Report()
{
    BudgetState& s = State();
    if (!s.dirty)
        return;
    s.dirty = false;
    fprintf(stderr,
            "[threads] machine: %u physical cores, %u logical cpus%s -> budget %u worker%s"
            "%s (reserve 2, committed 3, cap 6)\n",
            s.physical, s.logical, s.topologyOk ? "" : " (topology unreadable, halved)",
            s.total, s.total == 1 ? "" : "s",
            s.overridden ? " [CZ_WORKERS override]" : "");
    for (const Grant& g : s.grants)
        fprintf(stderr, "[threads]   %-10s %u of %u wanted (%s)\n", g.pool.c_str(),
                g.granted, g.desired, g.how);
    if (!s.grants.empty())
        fprintf(stderr, "[threads]   %u worker slot%s unclaimed\n", s.left,
                s.left == 1 ? "" : "s");
}
