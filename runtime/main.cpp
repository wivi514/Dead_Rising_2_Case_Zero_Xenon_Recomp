// Host runtime for the Dead Rising 2: Case Zero XenonRecomp port.
//
// Phase 1 boot sequence (docs/runtime-plan.md, docs/phase1-notes.md):
//   reserve guest memory + register the 57,822 recompiled functions (Memory::Init)
//   -> carve the guest heaps (GuestHeap::Init)
//   -> load the XEX image at its link base
//   -> publish the XEX headers and resolve data imports
//   -> run the entry point (0x825D9F30) on a bootstrapped guest thread
//      (PCR/TLS/TEB + stack)
//
// Kernel imports are HLE'd in kernel/imports.cpp; anything not yet implemented is a
// generated honest-failure stub (kernel/import_stubs.cpp) that logs its name and
// returns STATUS_NOT_IMPLEMENTED. The phase 1 gate is that the kernel-call sequence
// this prints is a prefix-match of Xenia's A1 capture — see kernel/klog.h and
// tools/kernel_call_diff.py.
//
// The phase 0.2 smoke harness that used to live here is now `--smoke`: it still
// walks and validates the whole PPCFuncMappings table, because that is what forces
// the linker to resolve every generated symbol, and losing it would quietly retire
// the phase 0 gate the moment phase 1 started failing for its own reasons.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <image.h> // XenonUtils: Image::ParseImage (devkit-key + LZX; see gotchas 15/16)

#include "cpu/thread_budget.h"
#include "cpu/crash_report.h"
#include "cpu/guest_thread.h"
#include "cpu/timebase.h"
#include "gpu/vk_renderer.h"
#include "host/first_run.h"
#include "host/host_paths.h"
#include "host/settings.h"
#include "host/window.h"
#include "kernel/audio.h"
#include "kernel/content.h"
#include "kernel/heap.h"
#include "kernel/memory.h"
#include "kernel/vfs.h"
#include "kernel/xex_imports.h"
#include "ppc_recomp_shared.h"

// kernel/file_imports.cpp. Declared here rather than in a header because the file
// layer's only other callers are the guest's own imports, and one arm does not earn a
// header of its own.
void FileImportsWriteSelfTest();

namespace {

std::vector<uint8_t> LoadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        fprintf(stderr, "runtime: cannot open %s\n", path);
        exit(1);
    }
    const std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

// The phase 0.2 gate, kept runnable. It is a LINK gate, not a behaviour test: the
// table holds a pointer to every one of the generated functions, so the linker
// cannot dead-strip any of them and an undefined `sub_XXXXXXXX` or
// `__imp__<KernelName>` is a hard error rather than something discovered halfway
// through a boot.
//
// The checks are the ones that can be made honestly with no guest running:
//   * the table is terminated and its length matches the recompiler's own count
//   * every guest address is inside the image, word-aligned, and strictly
//     increasing (a duplicate or out-of-order entry means the function list was
//     built from overlapping sources — exactly what the widening repairs in
//     finding 13 could have produced had they gone wrong)
//   * no host pointer is null
//   * the timebase calibrates, because a guest that reads `mftb` before that
//     happens divides by zero (gotcha 1)
int RunSmoke()
{
    constexpr uint64_t IMAGE_LO = PPC_IMAGE_BASE;
    constexpr uint64_t IMAGE_HI = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;

    int failures = 0;
    auto fail = [&](const char* what, size_t index, uint64_t addr) {
        if (++failures <= 20)
            fprintf(stderr, "  FAIL [%zu] 0x%08" PRIX64 ": %s\n", index, addr, what);
    };

    printf("Dead Rising 2: Case Zero — phase 0.2 smoke harness\n");
    printf("  image  0x%08" PRIX64 "..0x%08" PRIX64 "  (%.2f MB)\n", IMAGE_LO, IMAGE_HI,
           double(PPC_IMAGE_SIZE) / (1024.0 * 1024.0));
    printf("  code   0x%08llX + 0x%llX\n", (unsigned long long)PPC_CODE_BASE,
           (unsigned long long)PPC_CODE_SIZE);

    if (!cz_timebase::init())
    {
        fprintf(stderr, "FAIL: timebase calibration returned 0 — every guest `mftb` "
                        "would divide by zero.\n");
        return 1;
    }
    printf("  host TSC %.3f GHz -> guest timebase %.6f MHz\n",
           double(cz_timebase::host_hz) / 1e9, double(CZ_TIMEBASE_HZ) / 1e6);

    size_t count = 0;
    uint64_t prev = 0, lo = UINT64_MAX, hi = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->guest != 0 || m->host != nullptr;
         ++m, ++count)
    {
        const uint64_t addr = m->guest;
        if (m->host == nullptr)
            fail("null host function pointer", count, addr);
        if (addr < IMAGE_LO || addr >= IMAGE_HI)
            fail("guest address outside the image", count, addr);
        if (addr & 3)
            fail("guest address not word-aligned", count, addr);
        if (count != 0 && addr <= prev)
            fail("guest address not strictly increasing (duplicate or unsorted)", count, addr);
        prev = addr;
        if (addr < lo) lo = addr;
        if (addr > hi) hi = addr;
    }

    // Deliberately "mapping entries", not "functions". The table is larger than the
    // image's function count because it also maps the kernel import thunks and the
    // save/restore ladder helpers: 57,822 + 244 + 236 + `_xstart` = 58,303.
    // Reporting this as a function count invites the reader to conclude the image
    // grew.
    printf("  mapped %zu entries, 0x%08" PRIX64 "..0x%08" PRIX64 "\n"
           "         (guest functions + kernel import thunks + save/restore ladders)\n",
           count, lo, hi);

    if (count == 0)
    {
        fprintf(stderr, "FAIL: the mapping table is empty.\n");
        return 1;
    }
    if (failures > 20)
        fprintf(stderr, "  ... and %d more\n", failures - 20);
    if (failures != 0)
    {
        fprintf(stderr, "FAIL: %d bad mapping entries.\n", failures);
        return 1;
    }
    printf("OK: every generated symbol resolved and every mapping entry is sane.\n");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--smoke") == 0)
        return RunSmoke();

    // Where everything is, decided once and printed once. It used to be
    // "../../assets/game/default.xex" — CWD-relative, which is why every recipe in
    // CLAUDE.md begins with `cd runtime/build`. See host/host_paths.h.
    HostPaths::Report();

    const std::string xexDefault = HostPaths::GameXex().string();
    const char* xexPath = argc > 1           ? argv[1]
                          : getenv("CZ_XEX") ? getenv("CZ_XEX")
                                             : xexDefault.c_str();

    // THE FIRST-RUN CHECK, before anything else can fail for a reason that no longer
    // names the cause (release-plan A.2, host/first_run.h). It is placed here — after
    // the paths are decided, before the crash reporter, the memory map or any guest
    // code — so that its message is the first and only thing a player with a missing
    // package sees, rather than the twentieth line of a boot log.
    //
    // The renderer flag is CZ_VKDRAW, read the same way vk_renderer.cpp reads it: a
    // headless gate run does not need a shader cache and must not be refused for
    // lacking one.
    {
        const char* vk = getenv("CZ_VKDRAW");
        const bool renderer = vk && *vk && strcmp(vk, "0") != 0;
        if (!FirstRun::Gate(renderer, xexPath))
            return 1;
    }

    // Before anything can fault. A SIGSEGV in recompiled code otherwise reports only
    // a host backtrace, which names the guest function but not the address, object or
    // vtable slot involved — and the crash this was written for happens in ~2 runs in
    // 10, which is the worst case for attaching a debugger after the fact.
    CzInstallCrashReporter();

    // THE RENDERER'S COUNTERS, ON THE WAY OUT OF A HEADLESS RUN. Part 38 found that
    // closing the window skipped the counter dump and cost an operator evening's census
    // (gotcha 294); the headless half of that hole was still open. EVERY recipe in this
    // project ends a headless run with `timeout`, which is SIGTERM, and SIGTERM's
    // default action is to die silently — so the arm with the interesting numbers was
    // the one arm that never printed them, and the census had to be reconstructed from
    // per-draw logs every time. SDL's own signal handlers are disabled here already, so
    // this is the only handler in the process.
    //
    // _Exit rather than a graceful unwind for the same reason Shutdown() uses it: guest
    // threads are still executing recompiled code against guest memory, and running
    // static destructors underneath them would turn an ordinary exit into a crash.
    for (int sig : { SIGTERM, SIGINT })
        signal(sig, [](int s) {
            fprintf(stderr, "\n[host] signal %d — dumping renderer counters and exiting.\n",
                    s);
            ::VkRenderer_DumpStats();
            ::VkRenderer_SavePipelineCache();
            fflush(nullptr);
            std::_Exit(128 + s);
        });

    // Before any guest code runs: `mftb` is lowered to __rdtsc() scaled by this
    // calibration, and an uncalibrated timebase divides by zero (gotcha 1).
    if (!cz_timebase::init())
    {
        fprintf(stderr, "runtime: timebase calibration failed\n");
        return 1;
    }

    g_memory.Init();
    g_heap.Init();
    fprintf(stderr, "runtime: guest memory at %p, heaps ready\n", (void*)g_memory.base);

    // The thread budget, printed BEFORE anything spawns a worker. A performance number
    // taken at an unknown thread count is not comparable with anything, and this line is
    // what makes a run's parallelism visible in its own log rather than inferred from the
    // machine it happened to run on. See runtime/cpu/thread_budget.h.
    ThreadBudget_Report();

    // Before any guest code: the title reads the XMA context-array base out of the
    // decoder's register aperture exactly once (sub_8285EDF8) and caches it, so a
    // register published later than that is never seen.
    Audio_Init();

    // The package is the directory holding default.xex, and the guest reaches it as
    // both `game:` and `d:` — Xenia registers both symlinks before the title runs
    // (A1 logs them), so neither is something the guest asks for. Mounting before
    // any guest code runs is required: A1's 22nd distinct kernel call is already an
    // NtCreateFile on `game:\layout.bin`.
    {
        // parent_path(), not a hand-rolled split on '/'. The first version of this
        // searched for a forward slash only, so on Windows — where the path arrives as
        // C:\cz\...\assets\game\default.xex — it found none, fell back to ".", and
        // mounted `game:` and `d:` on the CURRENT DIRECTORY. The guest then failed
        // every file open and faulted through a null pointer several hundred
        // milliseconds later, with nothing in the crash report pointing back here.
        const std::string gameDir = std::filesystem::path(xexPath).parent_path().string();
        VfsSetGameRoot(gameDir);
        // Saves go in a SIBLING of the package directory, never inside it — see
        // kernel/content.cpp. Set up here rather than lazily so that a run whose save
        // directory cannot be created says so at startup instead of at the moment the
        // player tries to save.
        ContentSetRootFromGameDir(gameDir);
        // The graphics settings the resurrected PC options screen writes (part 60).
        // Loaded here — after the save root exists, BEFORE Host_WindowInit — because
        // the display mode is a window-creation decision. Env vars win over the file
        // at each consumer.
        Settings_Load(ContentSaveRoot() + "/cz_settings.txt");
    }

    // Load the XEX image into guest memory at its link base.
    //
    // Parsed from the raw .xex rather than from assets/game/default_image.bin, so
    // there is one loader in the project rather than two that can disagree. This
    // works only because our XenonRecomp checkout carries the devkit-AES-key patch
    // (docs/xenonrecomp-upstream-bugs.md) — stock XenonUtils returns an EMPTY image
    // with no diagnostic for this file, which reads as a corrupt XEX rather than a
    // wrong key (gotcha 15). The check below is what turns that into a real error.
    auto file = LoadFile(xexPath);
    Image image = Image::ParseImage(file.data(), file.size());
    if (image.size == 0 || image.sections.empty())
    {
        fprintf(stderr,
                "runtime: %s parsed to an EMPTY image (base=0x%zX size=0x%X, %zu "
                "sections).\n"
                "         That is the signature of a wrong AES key, not a corrupt "
                "file — this XEX uses the all-zero DEVKIT key, and stock XenonUtils "
                "hardcodes the retail one and fails silently. See gotcha 15 and "
                "docs/xenonrecomp-upstream-bugs.md.\n",
                xexPath, image.base, image.size, image.sections.size());
        return 1;
    }

    // Which sections get copied is decided by the BUFFER, not by a list of names.
    //
    // The loader points every section at `image.data.get() + VirtualAddress` inside one
    // `image.size`-byte decompressed image, so a section whose virtual range ends past
    // `image.size` would be memcpy'd out of somebody else's heap. In this XEX exactly
    // one does: `.reloc` is 0xB00200..0xBBA0D4 against an image size of 0xB40000.
    //
    // This used to be a NAME list — ".reloc, .XBLD, .edata, .idata" — introduced with
    // the comment "not read at runtime and, in this XEX, their source ranges over-run
    // the loaded image buffer". Only the first of those two claims applies to `.reloc`,
    // and NEITHER applies to `.idata`: it ends at 0xAF047A, comfortably inside the
    // buffer, and it is where this XEX keeps its RESOURCES —
    //
    //     Serial2  82AF0000  32b       Serial   82AF0080  63b
    //     Digest   82AF0100  28b
    //
    // — which the title reads through XexGetModuleSection. Leaving it unloaded made
    // `Digest` twenty-eight zero bytes, so the digest manager computed a perfectly
    // correct SHA-1 of `data/audio/Prologue.txt` and compared it against nothing (see
    // finding 50). A name list cannot state the condition it is standing in for, so
    // the next reader inherits the conclusion without the test; a bounds check IS the
    // condition, and it prints which sections it dropped and why.
    const uint64_t imageEnd = uint64_t(image.base) + image.size;
    auto skip = [&](const auto& s) {
        return !s.data || uint64_t(s.base) + s.size > imageEnd ||
               uint64_t(s.base) + s.size > PPC_MEMORY_SIZE;
    };
    for (const auto& s : image.sections)
    {
        if (skip(s))
            continue;
        memcpy(g_memory.base + s.base, s.data, s.size);
    }
    fprintf(stderr, "runtime: XEX loaded (%zu sections), entry=0x%zX\n",
            image.sections.size(), image.entry_point);
    for (const auto& s : image.sections)
        fprintf(stderr, "runtime:   section %-10s %08X..%08X flags=%u%s%s\n", s.name.c_str(),
                uint32_t(s.base), uint32_t(s.base) + s.size, unsigned(s.flags),
                s.data ? "" : " (no data)",
                skip(s) ? " (SKIPPED: runs past the loaded image buffer)" : "");

    // CZ_PEEK=<hexaddr>[,<words>]: dump guest memory as the XEX shipped it, before
    // any guest code runs. The use is differential — compare a value seen at a fault
    // against what was statically there, which separates "the guest wrote something
    // wrong" from "nothing ever wrote this".
    if (const char* peek = getenv("CZ_PEEK"))
    {
        const uint32_t addr = uint32_t(strtoul(peek, nullptr, 16));
        const char* comma = strchr(peek, ',');
        const int words = comma ? atoi(comma + 1) : 8;
        uint8_t* base = g_memory.base;
        fprintf(stderr, "[peek] %08X (as loaded):", addr);
        for (int i = 0; i < words; i++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(addr + 4 * i));
        fprintf(stderr, "\n");
    }

    // The XEX header block has to exist in guest memory before the data imports are
    // resolved: XexExecutableModuleHandle's storage is initialised to point at it,
    // and RtlImageXexHeaderField — the very FIRST kernel call A1 shows this title
    // making — walks it for real instead of returning a guessed 0.
    PublishXexHeaders(file.data(), file.size());
    ResolveXexDataImports(file.data());

    if (!g_memory.FindFunction(uint32_t(image.entry_point)))
    {
        fprintf(stderr, "runtime: entry point 0x%zX was not recompiled\n", image.entry_point);
        return 1;
    }

    // CZ_FILE_WRITE_SELFTEST=1 — the file layer's own create/write/read/verify round
    // trip, here because it needs guest memory (an OBJECT_ATTRIBUTES name is an
    // xpointer) and must not race the title's own file activity. Off by default and
    // free when off; see kernel/file_imports.cpp for why it exists at all.
    FileImportsWriteSelfTest();

    // The window, before any guest code runs.
    //
    // Order matters in one direction only: the first XE_SWAP can arrive within a
    // second of the guest starting, and a present seam that is not up yet would drop
    // frames that nobody would ever look for. Nothing here touches guest memory, so
    // there is no risk in the other direction.
    const bool haveWindow = Host_WindowInit();

    // Run the entry point as the main guest thread. Stack size is the XEX header's
    // own XEX_HEADER_DEFAULT_STACK_SIZE (0x40000 = 256 KB), which is also exactly
    // what Xenia gives its main XThread in A1 (70150000-70190000).
    GuestThreadParams params{};
    params.function = uint32_t(image.entry_point);
    params.stackSize = kDefaultGuestStackSize;

    // WHY THE GUEST ENTRY MOVED OFF THE PROCESS'S MAIN THREAD (phase 3).
    //
    // SDL's video subsystem has to be pumped from the thread that created the window,
    // and until phase 3 that thread was busy: main() called GuestThread::Run directly
    // and did not return for the life of the process. So one of the two had to move,
    // and it is the guest that moves, for two reasons.
    //
    // First, SDL's main-thread requirement is a hard platform rule elsewhere (macOS
    // will not deliver events off the main thread at all) and merely a
    // works-until-it-does-not on X11/Wayland. Putting the guest on a spawned thread
    // costs nothing and keeps that rule satisfied everywhere.
    //
    // Second, the guest side is already thread-agnostic and proven so: every one of
    // Case Zero's ~19 other guest threads has always run through this exact path
    // (GuestThreadHandle spawns a std::thread and calls GuestThread::Run on it). The
    // main guest thread is not special to the runtime — it is special to the TITLE,
    // which identifies threads by its own ids and never asks which host thread it is
    // on. What it must keep is its ORDER: it is still the first guest thread created,
    // so it still takes the first thread id.
    std::thread guest([params]() {
        const uint32_t exitCode = GuestThread::Run(params);
        fprintf(stderr, "runtime: guest entry returned (r3=0x%X)\n", exitCode);
        // Otherwise a title that exits leaves a live window in front of a process
        // with no guest in it, which from outside is indistinguishable from a hang —
        // and "finished" and "stuck" looking identical is the exact confusion finding
        // 37 was written about.
        Host_RequestQuit("the guest entry point returned");
    });

    if (haveWindow)
    {
        // Does not return: the window loop owns the process from here, and closing
        // the window exits it. The guest thread is deliberately not joined first —
        // this title never returns from its entry point (it parks at the title screen
        // waiting for input), so a join would be a wait for something that does not
        // happen.
        Host_WindowRun();
    }
    guest.join();
    return 0;
}
