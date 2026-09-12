#include "thread_budget.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
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
    bool noted = false;           // outside the budget (ThreadBudget_Note)
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

        // Core-count FLOOR (operator request, part 100; raised for 4-core parts in part
        // 107). The subtract-reserve-and-commit formula hands a 6-core machine only 1
        // worker and a 4-core machine 0, which leaves mid-range CPUs — the majority of
        // real players' machines — running the guards and record path almost serially.
        // Floor the budget so a machine with enough cores gets real parallelism
        // regardless of the reserve arithmetic:
        //   >= 6 physical cores -> at least 3 workers
        //   >= 4 physical cores -> at least 3 workers (part 107: the operator's
        //      instruction for the Ryzen 3 3100-class target — "make it so that 4 core
        //      cpu use 3 core as worker instead of 2"; a 4c/8t part has SMT siblings
        //      to run the third on, and the crowd there is pump-bound with the fence at
        //      zero, so a worker is worth more than the core it borrows from the OS)
        // Still clamped by the cap above (which the floor never exceeds), and CZ_WORKERS
        // below still overrides the whole thing.
        unsigned floor = 0;
        if (st.physical >= 4)
            floor = 3;
        if (budget < floor)
            budget = floor;

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

void ThreadBudget_Note(const char* pool, unsigned threads, const char* how)
{
    BudgetState& s = State();
    const char* name = pool ? pool : "?";
    for (Grant& g : s.grants)
        if (g.pool == name)
        {
            if (g.granted != threads)
            {
                g.granted = threads;
                s.dirty = true;
            }
            return;
        }
    Grant g;
    g.pool = name;
    g.desired = threads;
    g.granted = threads;
    g.how = how ? how : "outside the budget";
    g.noted = true;
    s.grants.push_back(g);
    s.dirty = true;
}

bool ThreadBudget_SetLowPriority(bool low)
{
    static const bool off = Env("CZ_NO_LOW_PRIORITY") != nullptr;
    if (off)
        return false;
#if defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), low ? THREAD_PRIORITY_BELOW_NORMAL
                                                     : THREAD_PRIORITY_NORMAL) != 0;
#elif defined(__linux__)
    // setpriority on a THREAD id adjusts that thread alone on Linux (nice is per-task
    // there, whatever POSIX says). The process's own nice is the baseline for "normal".
    static const int base = [] {
        errno = 0;
        const int n = getpriority(PRIO_PROCESS, 0);
        return errno ? 0 : n;
    }();
    const pid_t tid = pid_t(syscall(SYS_gettid));
    return setpriority(PRIO_PROCESS, id_t(tid), low ? base + 10 : base) == 0;
#else
    (void)low;
    return false;
#endif
}

void ThreadBudget_Report()
{
    BudgetState& s = State();
    if (!s.dirty)
        return;
    s.dirty = false;
    fprintf(stderr,
            "[threads] machine: %u physical cores, %u logical cpus%s -> budget %u worker%s"
            "%s (reserve 2, committed 3, floor 3@4c+, cap 6)\n",
            s.physical, s.logical, s.topologyOk ? "" : " (topology unreadable, halved)",
            s.total, s.total == 1 ? "" : "s",
            s.overridden ? " [CZ_WORKERS override]" : "");
    unsigned outside = 0;
    for (const Grant& g : s.grants)
        if (!g.noted)
            fprintf(stderr, "[threads]   %-12s %u of %u wanted (%s)\n", g.pool.c_str(),
                    g.granted, g.desired, g.how);
    if (!s.grants.empty())
        fprintf(stderr, "[threads]   %u worker slot%s unclaimed\n", s.left,
                s.left == 1 ? "" : "s");
    // The threads the budget does NOT count, so the block names everything that can be
    // runnable at once. A thread here is blocked except during a burst; the `how` says
    // which burst and at what priority.
    for (const Grant& g : s.grants)
        if (g.noted)
        {
            outside += g.granted;
            fprintf(stderr, "[threads]   %-12s %u outside the budget (%s)\n",
                    g.pool.c_str(), g.granted, g.how);
        }
    if (outside)
        fprintf(stderr,
                "[threads]   total runnable at a burst: %u budget + %u outside + pump + "
                "the guest's busy threads, on %u physical cores\n",
                s.total, outside, s.physical);
}

#if defined(_WIN32)
static void PinSelfByName(const char* name);
#endif
void ThreadBudget_NameSelf(const char* name)
{
#if defined(__linux__)
    char shortName[16];
    snprintf(shortName, sizeof shortName, "%s", name);
    pthread_setname_np(pthread_self(), shortName);
#elif defined(_WIN32)
    PinSelfByName(name);   // CZ_GUEST_PIN: the Windows sweep cannot read names, so the
                           // threads that have one pin (or confine) themselves here
#else
    (void)name;
#endif
}

// ---------------------------------------------------------------------------------------
// CZ_GUEST_PIN (part 118): the two guest threads that ARE the frame, each on a physical
// core of its own, with the SMT sibling kept empty.
//
// WHY. Part 118's PMU pair on the guest's Main Thread: with our renderer running it
// retires 0.78 cycles an instruction, without it 0.665 — 17% more cycles for the same
// instruction stream, of which the extra DRAM fills (31.1k vs 27.4k a frame) explain
// perhaps a third. The rest is either DRAM latency under our traffic or the scheduler
// landing the Main Thread on the SMT sibling of a core our pump, draw or guard thread
// is saturating. Pinning separates the two: if the Main Thread's cycles a frame fall
// when its core is exclusively its own, the sibling was busy; if they do not, it is the
// memory system and no placement fixes it.
//
// HOW. Every thread of the process inherits its creator's affinity, so the whole
// process is first confined to the CPUs OUTSIDE the two reserved cores (both siblings
// of each), from the main thread before anything is spawned; then, when the title
// names its "Main Thread" and "Draw Thread", each is moved onto its reserved core.
// The reserved cores are the first two physical cores in the affinity mask, with all
// their siblings read from sysfs (`thread_siblings_list`), so it is correct on any SMT
// width and a no-op on a machine without SMT topology. Off by default; `taskset -c 0-7`
// (the whole process on eight distinct cores) was measured WORSE in part 117 — this is
// not that: nothing else loses a CPU it was using, only the two siblings.
#if defined(__linux__)
namespace
{
struct PinPlan
{
    bool on = false;
    int mode = 0;                    // 1 = Main + Draw; 2 = also cz-pump and cz-draw
    int mainCpu = -1, drawCpu = -1, pumpCpu = -1, rendCpu = -1;
    std::vector<int> reserved;       // every reserved core and all its siblings
    cpu_set_t rest;                  // the process mask minus `reserved`
};

std::vector<int> SiblingsOf(int cpu)
{
    std::vector<int> out;
    const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                             "/topology/thread_siblings_list";
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return out;
    char buf[256] = {0};
    if (fgets(buf, sizeof buf, f))
    {
        // "0,8" or "0-1" or "0"
        for (char* p = buf; *p;)
        {
            char* end = nullptr;
            long a = strtol(p, &end, 10);
            if (end == p)
                break;
            long b = a;
            if (*end == '-')
                b = strtol(end + 1, &end, 10);
            for (long c = a; c <= b; ++c)
                out.push_back(int(c));
            while (*end == ',' || *end == '\n' || *end == ' ')
                ++end;
            p = end;
        }
    }
    fclose(f);
    if (out.empty())
        out.push_back(cpu);
    return out;
}

PinPlan& Plan()
{
    static PinPlan plan = [] {
        PinPlan p;
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof set, &set) != 0)
            return p;
        const char* e = Env("CZ_GUEST_PIN");
        if (e)
        {
            if (strcmp(e, "0") == 0)
                return p;
            p.mode = atoi(e) >= 2 ? 2 : 1;
        }
        else
        {
            // THE DEFAULT (part 118 campaign 2, three runs an arm, matched bands):
            // mode 2 on eight physical cores with SMT — where it was measured — and
            // off below that. Mode 2 reserves four cores; on six it would leave two
            // for the guard pool, the Havok workers and the audio pump, which nobody
            // has measured, and on a machine without SMT the sibling it keeps empty
            // does not exist (the migration half of the effect is unmeasured alone).
            const unsigned physical = ThreadBudget_PhysicalCores();
            bool smt = false;
            for (int cpu = 0; cpu < CPU_SETSIZE && !smt; ++cpu)
                if (CPU_ISSET(cpu, &set))
                    smt = SiblingsOf(cpu).size() > 1;
            if (physical >= 8 && smt)
                p.mode = 2;
            else
                return p;
        }
        // Distinct physical cores from the TOP of the mask downwards: cpu 0 is where
        // the kernel lands most interrupt handling (the GPU driver's included) and a
        // thread pinned there shares it.
        const int want = p.mode == 2 ? 4 : 2;
        std::vector<int> picked;
        std::set<int> taken;
        for (int cpu = CPU_SETSIZE - 1; cpu >= 0 && int(picked.size()) < want; --cpu)
        {
            if (!CPU_ISSET(cpu, &set))
                continue;
            int core = cpu;
            ReadInt("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/core_id",
                    &core);
            if (taken.count(core))
                continue;
            taken.insert(core);
            // Only a core WITH an SMT sibling qualifies: on a hybrid part (12700H) the
            // E-cores have none, sit at the top of the index range, and are the last
            // place to put the frame's longest thread.
            const std::vector<int> sibs = SiblingsOf(cpu);
            if (sibs.size() < 2)
                continue;
            for (int s : sibs)
                p.reserved.push_back(s);
            picked.push_back(cpu);
        }
        if (int(picked.size()) < want)
            return p;
        p.mainCpu = picked[0];
        p.drawCpu = picked[1];
        if (p.mode == 2)
        {
            p.pumpCpu = picked[2];
            p.rendCpu = picked[3];
        }
        // Everything but the reserved CPUs must remain — at least two — or the plan is
        // refused rather than starving the pool.
        int left = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &set) &&
                std::find(p.reserved.begin(), p.reserved.end(), cpu) == p.reserved.end())
                ++left;
        if (left < 2)
            return p;
        p.rest = set;
        for (int cpu : p.reserved)
            CPU_CLR(cpu, &p.rest);
        p.on = true;
        return p;
    }();
    return plan;
}
}   // namespace

void ThreadBudget_PinProcessAway()
{
    PinPlan& p = Plan();
    if (!p.on)
    {
        if (Env("CZ_GUEST_PIN") && strcmp(Env("CZ_GUEST_PIN"), "0") != 0)
            fprintf(stderr, "[pin] CZ_GUEST_PIN: refused (no SMT topology, too few CPUs, or no mask)\n");
        else
            fprintf(stderr, "[pin] thread placement OFF (%s; CZ_GUEST_PIN=1/2 forces it)\n",
                    Env("CZ_GUEST_PIN") ? "CZ_GUEST_PIN=0" : "under eight physical cores or no SMT");
        return;
    }
    const int rc = sched_setaffinity(0, sizeof p.rest, &p.rest);
    std::string res;
    for (int cpu : p.reserved)
        res += (res.empty() ? "" : ",") + std::to_string(cpu);
    fprintf(stderr, "[pin] thread placement mode %d (%s; CZ_GUEST_PIN=0 is the control): process "
                    "confined away from cpus {%s}; Main Thread -> cpu %d, Draw Thread -> cpu %d, "
                    "cz-pump -> %d, cz-draw -> %d (%s)\n",
            p.mode, Env("CZ_GUEST_PIN") ? "CZ_GUEST_PIN" : "the default from 8 physical cores with SMT",
            res.c_str(), p.mainCpu, p.drawCpu, p.pumpCpu, p.rendCpu,
            rc == 0 ? "ok" : strerror(errno));
}

void ThreadBudget_PinRest(std::thread::native_handle_type h)
{
    PinPlan& p = Plan();
    if (!p.on)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0)   // the process's own mask ...
        return;
    for (int cpu : p.reserved)                          // ... minus the reserved cores
        CPU_CLR(cpu, &set);
    // sched_getaffinity(0) reads the CALLING thread's mask, which on a pinned thread is
    // its one CPU: rebuild from the plan's own record of the process mask instead.
    if (CPU_COUNT(&set) < 2)
        set = p.rest;
    pthread_setaffinity_np(h, sizeof set, &set);
}

// Every thread of the process that is not one of the two pinned ones gets the rest
// mask. Threads are spawned from many places (std::thread in the renderer, the audio
// pump, the guest's own ExCreateThread) and a spawn inherits its creator's mask, so a
// per-spawn hook cannot be complete; this walk of /proc/self/task is, and at a dozen
// threads a second it is free. Run after every pin and from the [fps] window.
void ThreadBudget_PinSweep()
{
    PinPlan& p = Plan();
    if (!p.on)
        return;
    DIR* d = opendir("/proc/self/task");
    if (!d)
        return;
    // Which thread is which, by comm and CPU time. Several threads carry each name
    // (a spawn inherits its creator's comm until the title names it; children of the
    // pump inherit `cz-pump`), so the thread that IS the name is the one with the most
    // CPU time — the same rule every tool in tools/ uses. The first build of this sweep
    // exempted any thread sitting on one reserved CPU as "pinned", which exempted every
    // Havok worker the Main Thread had spawned onto its own core (frame 22 ms).
    struct Best { pid_t tid = -1; unsigned long cpu = 0; };
    Best main, draw, pump, rend;
    std::vector<pid_t> tids;
    while (dirent* e = readdir(d))
    {
        if (e->d_name[0] == '.')
            continue;
        const pid_t tid = pid_t(atoi(e->d_name));
        tids.push_back(tid);
        char path[64], comm[32] = {0};
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", int(tid));
        if (FILE* f = fopen(path, "r"))
        {
            if (!fgets(comm, sizeof comm, f))
                comm[0] = 0;
            fclose(f);
        }
        Best* b = nullptr;
        if (strncmp(comm, "Main Thread", 11) == 0)
            b = &main;
        else if (strncmp(comm, "Draw Thread", 11) == 0)
            b = &draw;
        else if (p.mode == 2 && strncmp(comm, "cz-pump", 7) == 0)
            b = &pump;
        else if (p.mode == 2 && strncmp(comm, "cz-draw", 7) == 0)
            b = &rend;
        if (!b)
            continue;
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", int(tid));
        unsigned long ut = 0, st = 0;
        if (FILE* f = fopen(path, "r"))
        {
            char line[512] = {0};
            if (fgets(line, sizeof line, f))
                if (char* close = strrchr(line, ')'))
                    sscanf(close + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
            fclose(f);
        }
        if (ut + st >= b->cpu)
        {
            b->cpu = ut + st;
            b->tid = tid;
        }
    }
    closedir(d);
    int moved = 0;
    for (pid_t tid : tids)
    {
        int cpu = -1;
        const char* who = nullptr;
        if (tid == main.tid) { cpu = p.mainCpu; who = "Main Thread"; }
        else if (tid == draw.tid) { cpu = p.drawCpu; who = "Draw Thread"; }
        else if (tid == pump.tid) { cpu = p.pumpCpu; who = "cz-pump"; }
        else if (tid == rend.tid) { cpu = p.rendCpu; who = "cz-draw"; }
        cpu_set_t cur;
        CPU_ZERO(&cur);
        if (sched_getaffinity(tid, sizeof cur, &cur) != 0)
            continue;
        if (cpu >= 0)
        {
            if (CPU_COUNT(&cur) == 1 && CPU_ISSET(cpu, &cur))
                continue;
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            if (sched_setaffinity(tid, sizeof one, &one) == 0)
                fprintf(stderr, "[pin] '%s' (tid %d) -> cpu %d\n", who, int(tid), cpu);
            continue;
        }
        bool touchesReserved = false;
        for (int r : p.reserved)
            if (CPU_ISSET(r, &cur))
                touchesReserved = true;
        if (!touchesReserved)
            continue;               // already on the rest mask (or a subset)
        if (sched_setaffinity(tid, sizeof p.rest, &p.rest) == 0)
            ++moved;
    }
    if (moved)
        fprintf(stderr, "[pin] sweep moved %d thread(s) off the reserved cores\n", moved);
}

bool ThreadBudget_PinNamedThread(const char* name, std::thread::native_handle_type h)
{
    PinPlan& p = Plan();
    if (!p.on)
        return false;
    int cpu = -1;
    if (strcmp(name, "Main Thread") == 0)
        cpu = p.mainCpu;
    else if (strcmp(name, "Draw Thread") == 0)
        cpu = p.drawCpu;
    if (cpu < 0)
        return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int rc = pthread_setaffinity_np(h, sizeof set, &set);
    fprintf(stderr, "[pin] '%s' -> cpu %d (%s)\n", name, cpu, rc == 0 ? "ok" : strerror(rc));
    ThreadBudget_PinSweep();
    return rc == 0;
}
#elif defined(_WIN32)
// THE WINDOWS SPELLING. Three differences from the Linux one, each forced by the API:
//   * a thread's affinity must be a SUBSET of the process mask, so the process mask is
//     left whole and every non-pinned thread is set to the rest mask individually;
//   * a new thread inherits the PROCESS mask, not its creator's, so the sweep (a
//     Toolhelp32 walk) is what keeps spawns off the reserved cores — plus the explicit
//     rest-mask at every spawn and NameSelf this runtime controls;
//   * there is no "get affinity" for a thread: SetThreadAffinityMask returns the previous
//     mask, so the sweep sets rather than compares.
// Topology from GetLogicalProcessorInformationEx(RelationProcessorCore): one record per
// physical core with its logical-processor mask and LTP_PC_SMT when it has a sibling.
// One processor group only (KAFFINITY is 64 bits; every machine this port has seen is
// under 64 logical processors).
#include <tlhelp32.h>
namespace
{
struct PinPlan
{
    bool on = false;
    int mode = 0;
    KAFFINITY reserved = 0;          // every reserved core's whole mask
    KAFFINITY rest = 0;              // the process mask minus `reserved`
    KAFFINITY mainMask = 0, drawMask = 0, pumpMask = 0, rendMask = 0;   // one bit each
    std::set<DWORD> pinnedTids;      // Main, Draw, cz-pump, cz-draw once known
};
std::mutex g_pinMutex;

int LowBit(KAFFINITY m)
{
    for (int i = 0; i < 64; ++i)
        if (m & (KAFFINITY(1) << i))
            return i;
    return -1;
}

PinPlan& Plan()
{
    static PinPlan plan = [] {
        PinPlan p;
        DWORD_PTR procMask = 0, sysMask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask))
            return p;
        const char* e = Env("CZ_GUEST_PIN");
        if (e && strcmp(e, "0") == 0)
            return p;
        // Cores: (highest logical index, mask, smt), from the topology.
        struct Core { int top; KAFFINITY mask; bool smt; };
        std::vector<Core> cores;
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        std::vector<uint8_t> buf(bytes ? bytes : 1);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
        if (!bytes || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes))
            return p;
        for (DWORD off = 0; off < bytes;)
        {
            auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
            if (rec->Size == 0)
                break;
            if (rec->Relationship == RelationProcessorCore && rec->Processor.GroupCount >= 1 &&
                rec->Processor.GroupMask[0].Group == 0)
            {
                const KAFFINITY m = rec->Processor.GroupMask[0].Mask & procMask;
                if (m)
                {
                    int top = -1;
                    for (int i = 63; i >= 0; --i)
                        if (m & (KAFFINITY(1) << i)) { top = i; break; }
                    cores.push_back({ top, m, (rec->Processor.Flags & LTP_PC_SMT) != 0 });
                }
            }
            off += rec->Size;
        }
        bool anySmt = false;
        for (const Core& c : cores)
            anySmt = anySmt || c.smt;
        if (e)
            p.mode = atoi(e) >= 2 ? 2 : 1;
        else if (cores.size() >= 8 && anySmt)
            p.mode = 2;     // the same default as Linux: eight physical cores with SMT
        else
            return p;
        // Cores with an SMT sibling only, highest index first (see the Linux note).
        std::sort(cores.begin(), cores.end(), [](const Core& a, const Core& b) { return a.top > b.top; });
        const int want = p.mode == 2 ? 4 : 2;
        std::vector<KAFFINITY> picked;
        for (const Core& c : cores)
        {
            if (int(picked.size()) >= want)
                break;
            if (!c.smt)
                continue;
            picked.push_back(c.mask);
            p.reserved |= c.mask;
        }
        if (int(picked.size()) < want)
            return p;
        auto one = [](KAFFINITY coreMask) { return KAFFINITY(1) << LowBit(coreMask); };
        p.mainMask = one(picked[0]);
        p.drawMask = one(picked[1]);
        if (p.mode == 2)
        {
            p.pumpMask = one(picked[2]);
            p.rendMask = one(picked[3]);
        }
        p.rest = procMask & ~p.reserved;
        int left = 0;
        for (int i = 0; i < 64; ++i)
            if (p.rest & (KAFFINITY(1) << i))
                ++left;
        if (left < 2)
            return p;
        p.on = true;
        return p;
    }();
    return plan;
}

void SetMask(HANDLE h, KAFFINITY m)
{
    if (h && m)
        SetThreadAffinityMask(h, DWORD_PTR(m));
}
}   // namespace

void ThreadBudget_PinProcessAway()
{
    PinPlan& p = Plan();
    if (!p.on)
    {
        if (Env("CZ_GUEST_PIN") && strcmp(Env("CZ_GUEST_PIN"), "0") != 0)
            fprintf(stderr, "[pin] CZ_GUEST_PIN: refused (no SMT topology, too few cores, or no mask)\n");
        else
            fprintf(stderr, "[pin] thread placement OFF (%s; CZ_GUEST_PIN=1/2 forces it)\n",
                    Env("CZ_GUEST_PIN") ? "CZ_GUEST_PIN=0" : "under eight physical cores or no SMT");
        return;
    }
    // The process mask stays whole (a thread's mask must be a subset of it); this
    // thread — and through NameSelf, PinRest and the sweep, every other — takes the rest.
    SetMask(GetCurrentThread(), p.rest);
    fprintf(stderr, "[pin] thread placement mode %d (%s; CZ_GUEST_PIN=0 is the control): threads "
                    "confined to mask %llx; Main Thread -> cpu %d, Draw Thread -> cpu %d, "
                    "cz-pump -> %d, cz-draw -> %d (Windows: per-thread masks, the process mask stays whole)\n",
            p.mode, Env("CZ_GUEST_PIN") ? "CZ_GUEST_PIN" : "the default from 8 physical cores with SMT",
            (unsigned long long)p.rest, LowBit(p.mainMask), LowBit(p.drawMask),
            p.mode == 2 ? LowBit(p.pumpMask) : -1, p.mode == 2 ? LowBit(p.rendMask) : -1);
}

void ThreadBudget_PinRest(std::thread::native_handle_type h)
{
    PinPlan& p = Plan();
    if (!p.on)
        return;
    SetMask(HANDLE(h), p.rest);
}

void ThreadBudget_PinSweep()
{
    PinPlan& p = Plan();
    if (!p.on)
        return;
    std::set<DWORD> pinned;
    {
        std::lock_guard lk(g_pinMutex);
        pinned = p.pinnedTids;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid || pinned.count(te.th32ThreadID))
                continue;
            HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE,
                                  te.th32ThreadID);
            if (!h)
                continue;
            SetMask(h, p.rest);
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

bool ThreadBudget_PinNamedThread(const char* name, std::thread::native_handle_type h)
{
    PinPlan& p = Plan();
    if (!p.on)
        return false;
    KAFFINITY m = 0;
    if (strcmp(name, "Main Thread") == 0)
        m = p.mainMask;
    else if (strcmp(name, "Draw Thread") == 0)
        m = p.drawMask;
    if (!m)
        return false;
    SetMask(HANDLE(h), m);
    {
        std::lock_guard lk(g_pinMutex);
        p.pinnedTids.insert(GetThreadId(HANDLE(h)));
    }
    fprintf(stderr, "[pin] '%s' -> cpu %d\n", name, LowBit(m));
    ThreadBudget_PinSweep();
    return true;
}

// Called from NameSelf: our own threads name themselves, so cz-pump and cz-draw pin
// themselves here in mode 2 and every other named thread of ours takes the rest mask.
static void PinSelfByName(const char* name)
{
    PinPlan& p = Plan();
    if (!p.on)
        return;
    KAFFINITY m = 0;
    if (p.mode == 2 && strcmp(name, "cz-pump") == 0)
        m = p.pumpMask;
    else if (p.mode == 2 && strcmp(name, "cz-draw") == 0)
        m = p.rendMask;
    if (m)
    {
        SetMask(GetCurrentThread(), m);
        std::lock_guard lk(g_pinMutex);
        p.pinnedTids.insert(GetCurrentThreadId());
        fprintf(stderr, "[pin] '%s' -> cpu %d\n", name, LowBit(m));
    }
    else
        SetMask(GetCurrentThread(), p.rest);
}
#else
void ThreadBudget_PinProcessAway() {}
bool ThreadBudget_PinNamedThread(const char*, std::thread::native_handle_type) { return false; }
void ThreadBudget_PinRest(std::thread::native_handle_type) {}
void ThreadBudget_PinSweep() {}
#endif
