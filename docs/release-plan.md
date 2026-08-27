# Release plan — GitHub releases for Windows, Linux and macOS (Apple Silicon)

> **STATUS: PLAN ONLY. Nothing here is built yet.** Written 2026-08-27, immediately after
> part 81 parked performance. The goal set by the operator: *"getting release for windows,
> linux and macos (for m1 and up) … that will not include game assets (user has to provide
> them themselves; we'll just make the assets folder empty and they'll have to put it in)."*
>
> Every claim about the current build below was measured or read out of the tree today, not
> remembered. Where a number has a shelf life it says so.

---

## §0 THE DECISION THAT COMES FIRST — "no game assets" is not the whole boundary

The instruction is to ship no game assets and let the user supply them. That is right, and it
is **not sufficient**, because two things in a working build are derived from the user's game
executable and neither of them lives in `assets/`:

| artifact | size | what it is derived from | how it gets used |
|---|---|---|---|
| **`ppc/`** | **155 MB of C++, 228 TUs, 57,822 functions** | `assets/game/default.xex` run through XenonRecomp | compiled into `libppc_image.a` (**162 MB**) and linked into `cz_runtime` |
| **`assets/shader_spv/`** | **449 SPIR-V modules, 13 MB** | the title's own Xenos shader microcode, translated by XenosRecomp + DXC | **loaded from disk at run time.** There is no runtime translation path — a shader missing from the cache prints one line and its draws are skipped |

So **a prebuilt `cz_runtime` binary contains the whole game's translated code**, and the
shader cache is the game's own shaders translated. Handing someone a binary and an empty
`assets/` folder does not make the release asset-free; it moves the game from `assets/` into
the executable, where it is less visible rather than less present.

This is a real fork and the whole plan hangs on it, so it is stated before any work.

### The three shapes a release can take

**(A) Conventional binary release.** Ship compiled `cz_runtime` per platform; the user
supplies only their STFS package. This is what the recomp scene does (UnleashedRecomp,
Zelda64Recomp). *Cost:* almost none beyond the platform ports. *Consequence:* the artifacts
are derivative works of the game executable and of its shaders. Also: **the release cannot be
built in public CI**, because CI has no XEX — every artifact would have to be built on your
machine and uploaded by hand, for three platforms, forever.

**(B) Build-from-your-own-copy release.** Ship source plus a one-shot bootstrap; the user's
machine does STFS extract → XEX image dump → XenonRecomp → compile 228 TUs → build the shader
cache. *Consequence:* no game-derived byte ever leaves your machine. *Cost:* the user needs a
full C++ toolchain and ~15-30 minutes of compiling, which on Windows is a real barrier — and
**the shader cache cannot be built this way at all today** (see §0b).

**(C) Split release — RECOMMENDED.** Ship the **host runtime** prebuilt (it contains no
game-derived code) and have the user's machine produce **only the game-derived part**, loaded
at run time as a module rather than linked in.

`runtime/CMakeLists.txt` already builds the recompiled image as its own static library
(`add_library(ppc_image STATIC …)`, "kept separate from the executable so that a runtime-only
edit does not risk a rebuild of 227 TUs"). **Turning that static library into a dynamically
loaded module is the enabling change for the whole release**, and it buys three things at
once:

1. the GitHub release artifacts contain **zero game-derived bytes**;
2. **the release can be built by GitHub Actions** on all three platforms, from a public repo,
   with no game data on the runner — which is what makes this sustainable rather than a
   manual chore three times per release;
3. a runtime-only fix ships as a small download instead of a rebuild of the user's image.

*Cost:* the user still needs a compiler for the one-time image build, and the module boundary
has to be designed (§3).

**Recommendation: (C), with (A) as the fallback if you decide the licence position is one you
are comfortable with.** The platform work in §1 and §2 is required by all three shapes, so it
can start before the fork is decided — and that is how this plan is ordered.

### §0b THE SHADER CACHE IS THE HARD HALF, AND IT BLOCKS (B) AND (C) TODAY

`ppc/` is reproducible on any machine from the user's own XEX: XenonRecomp is MIT, the config
is in the repo, and the pipeline is five documented tools. **The shader cache is not.**

* The shaders on disc are **not usable microcode** — that was retracted as finding 6: they are
  `.vo` objects carrying build metadata whose payloads share only background-noise overlap
  with what the guest actually submits.
* The 449 modules were accumulated over ~25 parts from Xenia `dump_shaders` captures **plus
  eleven operator play sessions**, including one complete playthrough. A user cannot
  reproduce that set from their own copy in any reasonable way.
* The renderer only loads prebuilt `.spv`. Grep confirms no `IDxcCompiler`, no XenosRecomp,
  nothing that translates at run time.

**So (B) and (C) need one piece of engineering that does not exist: translate microcode to
SPIR-V on demand, in-process, on first sight of an unknown shader.** That is §4, it is the
largest item in this plan, and it is worth doing on its own merits — it also permanently
retires the "the cache is complete" claim-with-a-shelf-life that CLAUDE.md warns about, and
the six-cache name-diff gate that goes with it.

An interim: the runtime already dumps microcode as the guest submits it (`CZ_SHADER_DUMP`),
independently of whether it can translate it. A first-run bootstrap could dump, translate and
cache — but the first minutes of play would render with holes, which is a bad first
experience and hard to explain in a README.

---

## §1 WHAT EACH PLATFORM ACTUALLY NEEDS — measured, per file

The good news, and it is better than expected: **the whole runtime's POSIX surface is five
files.** A census of platform headers across `runtime/`:

```
kernel/memory.cpp        <unistd.h> <sys/mman.h>
cpu/crash_report.cpp     <unistd.h> <dlfcn.h>
cpu/guest_thread.cpp     <unistd.h>
cpu/debug_tunables.cpp   <sys/stat.h>
gpu/vk_renderer.cpp      <unistd.h>          (one readlink("/proc/self/exe"))
```

Everything else is already `std::thread`, `std::filesystem`, `std::atomic` and SDL.

### 1.1 Linux — nearly done; the work is packaging, not porting

* **Ports needed: none.** This is the development platform.
* **Asset paths are CWD-relative** — the VFS resolves `game:` to `../../assets/game` and the
  shader cache tries `../../assets/shader_spv`, `../assets/shader_spv`, `assets/shader_spv`
  in turn. That works from `runtime/build/` and breaks from anywhere else. A release needs
  paths **anchored to the executable**, which the renderer already half-does via
  `readlink("/proc/self/exe")` — generalise that into one `HostPaths` helper (§3.2) and use
  it everywhere.
* **Bundle or depend?** SDL2, Vulkan loader and libavcodec are hard `find_package`
  requirements at configure time, deliberately, so a silently-missing one is a build message
  rather than a mystery in the game. For a release: ship an **AppImage** (bundles SDL2 and
  ffmpeg, keeps the system Vulkan loader and ICD, which is correct — the loader must be the
  host's) plus a plain tarball for distro users.
* **ffmpeg licence: this box's ffmpeg is `--enable-gpl`.** Shipping that makes the whole
  bundle GPL, which collides with this repo's PolyForm Noncommercial licence. The release
  must bundle an **LGPL** ffmpeg built with only what is needed (`libavcodec`, `libavutil`,
  the XMA2 decoder), dynamically linked, with the LGPL notice and the build recipe published.

### 1.2 Windows — five files, plus one genuinely fiddly mapping

* **`kernel/memory.cpp` — the hard part, and it is a known emulator pattern.** Today: a 4 GB
  `MAP_NORESERVE` anonymous reservation, then a `memfd_create` shared 512 MB region mapped
  `MAP_FIXED` at **three** addresses (`0xA0000000`, `0xC0000000`, `0xE0000000`) so the guest's
  three views of one physical range alias each other. Windows equivalent: reserve with
  `VirtualAlloc2(… MEM_RESERVE_PLACEHOLDER)`, split placeholders, and
  `MapViewOfFile3(… MEM_REPLACE_PLACEHOLDER)` a `CreateFileMapping(INVALID_HANDLE_VALUE)`
  section three times. Requires Windows 10 1803+. Xenia does exactly this, so the pattern is
  proven.
* **`cpu/crash_report.cpp`** — `sigaction(SIGSEGV/SIGBUS/SIGILL/SIGTRAP)` →
  `AddVectoredExceptionHandler`; `dladdr` → DbgHelp `SymFromAddr`. Keep the guest-state
  report identical: it is the thing that makes a fault diagnosable, and its host `pc` field
  is the one that is never stale.
* **`gpu/vk_renderer.cpp`** — `readlink("/proc/self/exe")` → `GetModuleFileNameW`. Folded into
  the `HostPaths` helper anyway.
* **`cpu/guest_thread.cpp`, `cpu/debug_tunables.cpp`** — trivial (`stat` → `std::filesystem`).
* **Toolchain: clang is not optional.** `ppc_context.h` uses `__builtin_assume`, and the
  CMakeLists says in as many words that every one of the 57,822 generated bodies fails under
  GCC. Use **clang-cl** with the MSVC ABI (so vcpkg's SDL2/ffmpeg binaries link) rather than
  MinGW.
* **Dependencies** via **vcpkg** (`sdl2`, `ffmpeg[avcodec,avutil]`, `vulkan-headers`); the
  Vulkan **loader** comes from the user's driver, as on Linux.
* **Risk: none identified that is worse than tedious.** No inline asm, no Linux-only threading.

### 1.3 macOS on Apple Silicon — the two real unknowns

**The biggest risk is already retired.** `ppc_context.h` maps the guest's VMX unit onto
**SIMDe** (`simde__m128`, `simde_mm_*`), not raw SSE intrinsics — so the recompiled code is
portable to ARM64 by construction, and SIMDe lowers it to NEON. Had it been raw `<xmmintrin.h>`
this platform would have been a rewrite rather than a port.

Concrete work:

* **`-msse4.1 -mavx` are hardcoded** on `ppc_image` and must become conditional on the target
  architecture. Note the CMakeLists' warning that without SSE4.1 "simde silently falls back to
  scalar emulation, which is correct but very slow" — the ARM64 equivalent question is whether
  SIMDe's NEON lowering covers the packs and `min/max_epu32` the saturating vector arithmetic
  uses, or falls back to scalar. **That is a measurable spike, not a guess** (§5 step 1).
* **16 KB pages.** Apple Silicon's page size is 16384, and `memory.cpp` does
  `mprotect(base, 0x1000, …)` for the guest null page. A 4 KB mprotect on a 16 KB page host
  fails, or over-protects three pages that the guest expects to be writable. This is a
  concrete bug waiting at first run, and it is the kind that presents as a mystery.
* **`memfd_create` → `shm_open`/`ftruncate`/`shm_unlink`** for the three aliased physical
  views. `MAP_FIXED` over an existing anonymous reservation works on macOS.
* **`dlfcn.h`, `sigaction` and the rest are native.** `SIGBUS` matters more here than on Linux.
* **Vulkan is MoltenVK, and this is the genuine unknown.** The renderer uses **dynamic
  rendering** (`vkCmdBeginRendering`, core 1.3), a **bindless, update-after-bind descriptor
  array** for the guest texture heap (~1,400 images), and **ray query** for the parked RT
  path. MoltenVK supports dynamic rendering and descriptor indexing, but Metal's argument
  buffer limits are the thing to check before promising anything, and `VK_KHR_ray_query` is
  simply absent — the RT path must compile out cleanly (it is already off by default and
  parked, so this is a build-flag question, not a feature loss).
* **Distribution:** arm64-only per the instruction (M1 and up). Ship a signed and notarised
  `.app` in a `.dmg`; without notarisation Gatekeeper will refuse it and the bug report will
  be "it doesn't open". Needs an Apple Developer account — **a decision with a fee attached,
  flagged now rather than at release time.**

---

## §2 THIRD-PARTY LICENSING FOR REDISTRIBUTION — do this before the first artifact

Building for yourself and shipping to others are different licence questions, and this repo
has never had to answer the second one.

| component | licence | what shipping requires |
|---|---|---|
| this repo | **PolyForm Noncommercial 1.0.0** | fine, but see the ffmpeg row — a GPL bundle would conflict |
| XenonRecomp / XenosRecomp | MIT | attribution in the release notes |
| SDL2 | zlib | attribution |
| **ffmpeg (libavcodec/libavutil)** | LGPL, **or GPL if built `--enable-gpl`** | **the local build is `--enable-gpl`.** Ship an LGPL build, dynamically linked, with notice + build recipe |
| MoltenVK | Apache 2.0 | attribution (macOS only) |
| o1heap | MIT | attribution |
| simde | MIT | attribution |
| DXC (if embedded for §4) | LLVM Exceptions to Apache 2.0 | attribution |
| UnleashedRecomp | **GPLv3 — structural reference only, no code** | nothing to ship; keep it that way |

**Add a `THIRD_PARTY.md` and generate the release's attribution block from it**, so it cannot
drift from what is actually linked.

---

## §3 THE RELEASE LAYOUT AND THE FIRST-RUN EXPERIENCE

### 3.1 What the user unpacks

```
CaseZeroRecomp/
  cz_runtime(.exe)            the host runtime — NO game-derived code
  lib/                        bundled SDL2, ffmpeg (LGPL), MoltenVK on macOS
  assets/
    package/    (EMPTY)       <- the user drops their STFS package here
    game/       (EMPTY)       <- tools/extract_stfs.py fills this
    shader_spv/ (EMPTY)       <- filled by §4, or by the bootstrap
    save/                     created on first run
  tools/                      extract_stfs.py, the image dumper, the image builder
  README.md  THIRD_PARTY.md  LICENSE
```

Each empty directory carries a `PUT_YOUR_GAME_HERE.txt` naming exactly what goes in it and
where it comes from. An empty directory in a zip is easy to lose; a directory with a file in
it is not.

### 3.2 One `HostPaths` helper, and it is a prerequisite for every platform

Today the game root and the shader cache are found by walking `../../assets/…` relative to the
**current directory**, which is why every documented command starts with `cd runtime/build`.
A shipped build must anchor to the **executable's** location:
`readlink("/proc/self/exe")` (Linux) / `GetModuleFileNameW` (Windows) /
`_NSGetExecutablePath` (macOS), with a `CZ_ROOT` override. Small, mechanical, and it removes
a whole class of "works here, not there" bug reports.

### 3.3 First run must refuse clearly, not fail mysteriously

The runtime's existing discipline is that a missing dependency is a **build** message rather
than a silent defect at play time. Extend that to missing user-supplied data: on start,
check for the package, the extracted game and the shader cache, and if any is missing print
what is missing, where it goes and which command produces it — then exit. **A black screen
with one skipped-shader log line is exactly the failure this project has spent parts of its
life diagnosing; a shipped build must not hand that to a stranger.**

---

## §4 THE SHADER PIPELINE — the largest item, and the one that decides (B)/(C)

Translate Xenos microcode → SPIR-V **in-process, on first sight**, replacing the prebuilt
`assets/shader_spv/` cache with one the runtime fills itself.

* Link XenosRecomp (MIT, already patched locally) as a library and embed DXC, or teach the
  path to emit SPIR-V directly.
* Key the on-disk cache by the microcode's FNV-1a hash — **which the renderer already
  computes and already logs** (`[imload] VS va=… hash=… size=…`), so the cache key and its
  self-check exist today.
* Translate off the pump thread, with the current "decline the draw and count it" behaviour as
  the fallback for the frame or two before a new shader is ready.
* **The gate is free and it already exists:** build a cache the new way from the 449 known
  microcode blobs in `~/DR2CZ-troubleshooting/ucode-dumps` and **diff the SPIR-V against the
  449 modules already on disk.** Byte-identical output on 449 of 449 is a stronger check than
  any picture test, and a disagreement names the shader.
* **What it retires:** the six variant caches drifting apart (which cost three parts once when
  the play cache was ten modules short), the name-diff gate, and "the cache is complete" as a
  claim with a shelf life.

---

## §5 ORDER OF WORK, AND WHY

Each step ends in something checkable. The first two are worth doing whichever release shape
you choose, so **the §0 fork does not have to be decided today.**

1. **The macOS ARM64 spike — half a day, and it is first because it can refute the plan.**
   Compile `ppc_image` alone for arm64 with SIMDe and no SSE flags. Answer two questions:
   does it build, and does SIMDe lower the VMX packs and `min/max_epu32` to NEON or to
   scalar? **Pre-registered kill: if the vector unit falls back to scalar, macOS is a
   different project** — say so before anything is promised. Nothing else is worth starting
   until this is known.
2. **`HostPaths` + the first-run check (§3.2, §3.3).** Platform-independent, needed by every
   shape, and it makes the current build runnable from outside `runtime/build/`.
3. **The Windows port** — the five files, clang-cl, vcpkg. Gate: `--smoke` passes, then the
   headless DebugJump route reaches the crowd, then `tools/part80_trace_band.py` says the
   frame is in the same regime as Linux. **A platform port is a same-binary A/B against
   itself on another OS**, and this project already owns the reader for it.
4. **The macOS port** — the same files plus 16 KB pages, `shm_open`, and the MoltenVK spike
   (bindless heap size, dynamic rendering, RT compiled out). Same three gates.
5. **CI: GitHub Actions matrix** — `ubuntu-latest`, `windows-latest`, `macos-14` (arm64).
   Note it can only build the **host** runtime, since no runner has an XEX — which is the
   practical argument for shape (C) restated as a fact about the build system.
6. **The shader pipeline (§4)** — the big one, gated on the 449-of-449 SPIR-V diff.
7. **`ppc_image` as a loadable module** — only if (C) is chosen. Design the ABI, then the
   one-time user-side image build (`cz_build_image` wrapping extract → dump → recompile →
   compile).
8. **Packaging + release automation** — AppImage / `.zip` / notarised `.dmg`, a
   `release-please`-style tag → build → attach flow, and `THIRD_PARTY.md` generated rather
   than written by hand.

## §6 EXPLICITLY OUT OF SCOPE

* **x86 macOS and Intel Macs.** The instruction is M1 and up. Rosetta 2 would run an x86_64
  build but MoltenVK under translation is not a configuration worth supporting.
* **32-bit anything, and Steam Deck / handheld packaging.** Later, if ever.
* **Shipping the game.** Not negotiable and not a judgement call — the package, the extracted
  data, `ppc/` and the shader cache are all game-derived, and §0 exists so that stays true of
  the artifacts as well as of the repo.
* **Performance work.** Parked as of part 81. Note that two changes are live and ON by
  default whose milliseconds were never measured (`docs/part82-kickoff.md` §0) — **a release
  is the first time a stranger runs those**, so the bind batch and the device command table
  should be on the pre-release checklist as the first bisection for any picture complaint.
