# Release programme — Windows, Linux, macOS (Apple Silicon)

> **THE DECISION, taken 2026-08-27.** *"We'll make it already recompiled for release to make
> it easiest for the player. Just drop in the game."* — **shape (A)**: ship a compiled
> `cz_runtime` per platform containing the recompiled image; the player supplies their own
> STFS package and nothing else.
>
> **STATUS as of 2026-08-27 (part 82): MILESTONE A IS COMPLETE AND GATED, AND D.1 IS DONE
> AND RETRACTS §1.4.** What was NOTHING BUILT YET is now a Linux artifact that runs in a
> clean container, refuses honestly when the game is missing, and carries no GPL. §9 at the
> bottom of this file is the execution record, item by item, with what each gate measured.
>
> Every number in §1 was measured with the tool named beside it — **except §1.4, whose
> vertex-shader half is RETRACTED in place below.** Everything else is an estimate and says
> so.
>
> The predecessor of this file was a decision document weighing three release shapes. That
> comparison is gone now the decision is made — it survives in git history and in
> `docs/part82-kickoff.md`.

---

## §1 THE EVIDENCE THIS PLAN IS BUILT ON — measured today, not assumed

Four things were checked before the plan was ordered, because three of them could have
changed it and one of them did.

**1.1 The runtime's entire non-portable surface is five files.** A census of platform headers
across `runtime/`:

```
kernel/memory.cpp        <unistd.h> <sys/mman.h>   the 4 GB map + three aliased physical views
cpu/crash_report.cpp     <unistd.h> <dlfcn.h>      sigaction handlers, dladdr symbolisation
cpu/guest_thread.cpp     <unistd.h>
cpu/debug_tunables.cpp   <sys/stat.h>
gpu/vk_renderer.cpp      <unistd.h>                one readlink("/proc/self/exe")
```

Everything else is `std::thread`, `std::filesystem`, `std::atomic` and SDL.

**1.2 The ARM64 architecture risk is retired, and most of it was already handled upstream.**
This was written into the previous plan as step 1's pre-registered kill — *"if the vector unit
falls back to scalar, macOS is a different project."* It does not.

* **SIMDe lowers every VMX operation the build calls "not optional" to NEON.** Compiled both
  targets freestanding (no sysroot needed) and disassembled:

  | op | aarch64 instrs | verdict | | op | aarch64 instrs | verdict |
  |---|---|---|---|---|---|---|
  | `min_epu32` | 2 | NEON | | `packs_epi16` | 3 | NEON |
  | `max_epu32` | 2 | NEON | | `packus_epi16` | 3 | NEON |
  | `blendv_ps` | 3 | NEON | | `packs_epi32` | 3 | NEON |
  | `shuffle_epi8` | 4 | NEON | | `cvtps_epi32` | 8 | NEON |
  | `dp_ps` | 5 | NEON | | `movemask_ps` | 9 | NEON |

  **Ten of ten, zero scalar fallbacks.** The reproducer is `tools/arm64_spike/run.sh` —
  it compiles freestanding so it needs no aarch64 sysroot, exits 1 on any scalar fallback,
  and should be re-run natively on the Mac as item C.0. **It proves the LOWERING, not that
  the full image builds; those are different claims and §1.3 is the second one.**
* **The generated code contains no raw x86 intrinsics at all.** Census over all 229 files in
  `ppc/`: `__builtin_ia32` 0, `immintrin` 0, `x86intrin` 0, and every `_mm_*` / `__m128*`
  token is `simde_`-prefixed. The only `__x86_64__` guard in the whole image is in
  `ppc_context.h`.
* **And that guard already has an aarch64 arm.** `ppc_context.h:260` handles the one thing
  SIMDe does not — denormal and rounding-mode control — with an `#elif defined(__aarch64__)`
  branch driving ARM's FPCR (`RoundShift 22`, FZ/FZ16 at bits 19/24). Likewise
  `cpu/timebase.h` is already `#if defined(__x86_64__)`-guarded and `cpu/timebase.cpp`
  already carries `mrs %0, cntvct_el0` for aarch64.

  **So the macOS risk is the OS, not the architecture** — 16 KB pages, `shm_open`, MoltenVK
  and notarisation. That is a different and smaller list than the plan assumed this morning.
  *(Upstream nit, harmless: the aarch64 `GuestToHost` comment reads "Nearest, Zero,
  -Infinity, -Infinity"; the code is right and the third entry is +Infinity.)*

**1.3 What could NOT be settled here, stated so nobody quotes a rigged result.** A full
generated TU compiled cleanly for x86_64 in 0.96 s but **could not be compiled for aarch64 on
this machine** — there is no aarch64 sysroot, and borrowing the x86_64 glibc headers produced
a cascade of `uint64_t`-is-`unsigned long long`-not-`unsigned long` type errors that are
artifacts of the hack and say nothing about the code. **That half of the spike needs a real
Mac or a real sysroot and is item §3.C.0.**

**1.4 The PIXEL shader cache is derivable from the player's own disc. THE VERTEX HALF IS
NOT — the claim below is RETRACTED, and D.1 replaced the measurement.**

~~`tools/vo_microcode_probe.py` over the 1,571 shader objects in the three prologue banks,
against the 449-blob ucode oracle:~~

```
PS: 335 of 335 verbatim (100.0%)
VS:  81 of 103 verbatim + 16 tail-matched (head patched at load), 4 partial, 2 absent
RECOVERABLE: 432 of 438 = 98.6%          <- the VS line and the total are WRONG
```

**What is actually true, measured by FULL containment rather than by a 48-byte head probe
(`tools/vo_extract_microcode.py`, D.1):**

```
PS: 343 of 345 EXACT, byte-for-byte, reproduced from the disc by a decoded container
VS:   0 of 104 exact.  95 of 104 match by their last 48 bytes and differ in 3-35
                       SCATTERED bytes throughout
```

**Zero.** The old test's positive was too weak to mean what it was read to mean: a 48-byte
head is a *shared vertex-shader prologue*, and two different runtime shaders match the same
disc object at the same offset for exactly 48 bytes before diverging. Aligned by their tails,
the differing bytes come in groups of three dwords with whole fields zeroed on disc —

```
dw13  disc 00000A88   runtime 00393A88
dw14  disc 00000000   runtime 00000003
dw15  disc 05F82000   runtime 03F82000
```

— which is **the title patching vertex FETCH instructions at load out of the vertex
declaration**, standard Xbox 360 practice. Those fields are exactly what decides the vertex
format XenosRecomp emits, so a vertex shader **cannot be pre-translated from the disc at
all.** §3.D is re-planned around that below.

Finding 6's retraction — that the banks contain microcode — **stands**, and is in
`xenia-capture-analysis.md` §6. **The disc holds 1,265 DISTINCT PIXEL SHADERS against the
345 accumulated over 25 parts and eleven operator sessions**, so for the pixel half "the
cache is complete" stops being a claim with a shelf life. It holds 142 distinct vertex
shaders too, but as templates rather than as usable microcode.

---

## §2 WHAT SHIPS

### 2.1 The artifacts

| platform | artifact | built on | notes |
|---|---|---|---|
| Linux x86_64 | `CaseZeroRecomp-linux-x86_64.AppImage` + `.tar.zst` | this machine | AppImage bundles SDL2 + ffmpeg; Vulkan **loader stays the host's** |
| Windows x86_64 | `CaseZeroRecomp-windows-x86_64.zip` | this machine, clang-cl cross or a Windows box | vcpkg SDL2 + ffmpeg beside the exe |
| macOS arm64 | `CaseZeroRecomp-macos-arm64.dmg` | a Mac (unavoidable — notarisation) | signed + notarised `.app`, MoltenVK inside |

**No `assets/` content in any artifact.** With the first-run shader build (§3.D) that includes
no `.spv` either, so the only game-derived thing shipped is the recompiled image inside the
executable — which §1 of `part82-kickoff.md` states plainly rather than leaving implied.

### 2.2 The layout the player unpacks

```
CaseZeroRecomp/
  cz_runtime(.exe)              the host runtime + the recompiled image
  lib/                          bundled SDL2, ffmpeg (LGPL build), MoltenVK on macOS
  tools/                        extract_stfs.py + the image dumper (for the drop-in step)
  assets/
    package/PUT_YOUR_GAME_HERE.txt
    game/       (created by the first-run extract)
    shader_spv/ (created by the first-run shader build)
    save/       (created on first run)
  README.md  THIRD_PARTY.md  LICENSE
```

Every empty directory carries a `PUT_YOUR_GAME_HERE.txt` naming what goes in it and where it
comes from. **An empty directory in a zip is easy to lose; a directory with a file in it is
not.**

### 2.3 The player's first run, end to end

1. Drop the STFS package into `assets/package/`.
2. Launch. The runtime finds no `assets/game/`, runs the STFS extract itself (it is 825 MB in,
   832 MB out, ~30 s), and reports progress.
3. It finds no shader cache and builds one from the disc banks — **"Preparing shaders",
   ~1,571 of them, parallel across cores** (§3.D).
4. It pre-warms the pipeline cache in the same screen. Part 71 measured a *missing* pipeline
   cache at **17.8 s of stutter spread through play**; this moves that into the progress bar
   where a player expects it.
5. Game starts. Subsequent launches skip 2-4 entirely.

**If any step cannot proceed it must say what is missing, where it goes, and which command
produces it — then exit.** A black screen with one skipped-shader log line is the failure this
project has spent parts of its life diagnosing, and a shipped build must not hand that to a
stranger.

---

## §3 THE WORK, IN ORDER

Five milestones. A, B and E are required; C is macOS; D is required for the first-run
experience and is the biggest single item. Estimates are working days and are estimates.

### MILESTONE A — make the tree shippable at all — **COMPLETE (part 82); see §9.1**

**A.1 `HostPaths` — anchor everything to the executable, not the CWD. DONE (part 82).**
Today the VFS resolves `game:` to `../../assets/game` and the shader cache tries three
`../..`-relative candidates, which is why every documented command begins with
`cd runtime/build`. Replace with one helper: `/proc/self/exe` (Linux), `GetModuleFileNameW`
(Windows), `_NSGetExecutablePath` (macOS), plus a `CZ_ROOT` override.
**Gate:** every existing headless recipe passes from three different working directories.

**A.2 The first-run detector and its refusal messages. DONE (part 82).**
Check package → extracted game → shader cache, in order; each missing one prints what and
where and exits non-zero. **Gate:** four runs — nothing present, package only, game only,
all present — each producing the right message. Break each check on purpose and confirm it
fires (gotcha 30).

**A.3 A release build type that is not `RelWithDebInfo`. DONE (part 82).**
The default exists for `addr2line` on the recompiled image and is right for development.
A release wants `-O2`, no `-g`, stripped, with a separate `.debug` artifact kept locally so a
player's crash report is still resolvable.
**Gate:** `--smoke`, then the crowd route, then `part80_trace_band.py` says the frame is in
the same regime as the dev build — **a build-type change is a performance change until
measured.**

**A.4 Bundle the runtime deps and prove the bundle is what loads. DONE (part 82).**
**Gate:** `ldd`/`otool -L`/Dependency Walker on the packaged binary in a clean container or
VM with no dev packages installed. A dependency that silently resolves to a system copy on
the build machine is the classic packaging defect.

### MILESTONE B — Windows (~5 days)

**B.1 `kernel/memory.cpp`.** *(2 days — the only genuinely fiddly port item)*
Today: a 4 GB `MAP_NORESERVE` reservation, then a `memfd_create` 512 MB region mapped
`MAP_FIXED` at **three** addresses (`0xA0000000`, `0xC0000000`, `0xE0000000`) so the guest's
three views of one physical range alias. Windows: `VirtualAlloc2` with
`MEM_RESERVE_PLACEHOLDER`, split the placeholders, then `MapViewOfFile3` a
`CreateFileMapping(INVALID_HANDLE_VALUE)` section three times with `MEM_REPLACE_PLACEHOLDER`.
Requires Windows 10 1803+. **Xenia does exactly this, so the pattern is proven.**
**Gate:** write through `0xA0000000`, read back through `0xC0000000` and `0xE0000000` — the
aliasing is the whole point and it is one unit test. Then `--smoke`, then the crowd route.

**B.2 `cpu/crash_report.cpp`.** *(1.5 days)*
`sigaction` → `AddVectoredExceptionHandler`; `dladdr` → DbgHelp `SymFromAddr`. **Keep the
guest-state report identical** — its host `pc` is the field that is never stale and is what
makes a fault diagnosable at all. **Gate:** fault on purpose in a known guest function and
confirm the report names it.

**B.3 The other three files + build.** *(1 day)* `readlink` → A.1's helper; `stat` →
`std::filesystem`. **clang-cl with the MSVC ABI**, not MinGW, so vcpkg's SDL2/ffmpeg link.
Clang is not optional: `ppc_context.h` uses `__builtin_assume` and the CMakeLists says every
one of the 57,822 generated bodies fails under GCC.

**B.4 Gates.** *(0.5 day)* `--smoke`; the headless DebugJump route reaches the crowd;
`part80_trace_band.py` against the Linux build. **A platform port is a same-binary A/B against
itself on another OS, and this project already owns the reader for it.**

### MILESTONE C — macOS on Apple Silicon (~5 days, plus procurement)

**C.0 Finish the spike on real hardware — do this first, it is half a day.**
§1.3 says what could not be answered here. On a Mac: build `ppc_image` alone for arm64 with no
SSE flags, re-run the ten-op SIMDe disassembly natively, and time a full 228-TU build.
**Nothing else in this milestone starts until that builds.**

**C.1 16 KB pages.** *(0.5 day)* Apple Silicon's page size is 16384 and `memory.cpp` does
`mprotect(base, 0x1000, …)` for the guest null page. A 4 KB request either fails or
over-protects three pages the guest expects writable. **This is a concrete bug waiting at
first run and it would present as a mystery.** Use `sysconf(_SC_PAGESIZE)` and decide
deliberately what "the null page" means when it is 16 KB wide.

**C.2 `memfd_create` → `shm_open`/`ftruncate`/`shm_unlink`.** *(0.5 day)* Same three aliased
views; `MAP_FIXED` over an anonymous reservation works on macOS. Same aliasing unit test as
B.1.

**C.3 The MoltenVK spike — the real unknown.** *(2 days, and it can force a redesign)*
The renderer uses **dynamic rendering** (`vkCmdBeginRendering`, core 1.3), a **bindless,
update-after-bind descriptor array** for the guest texture heap (~1,400 images), and **ray
query** for the parked RT path. MoltenVK covers the first two to a degree; **Metal's argument
buffer limits are the thing to check before promising anything**, and `VK_KHR_ray_query` is
simply absent.
**Answer three questions in this order:** does the bindless heap fit; does dynamic rendering
work as used; does the RT path compile out cleanly (it is already off by default and parked,
so this is a build-flag question, not a feature loss).
**Pre-registered kill: if the texture heap does not fit Metal's argument buffer limits,
macOS needs a descriptor redesign and is its own milestone — say so rather than absorbing it.**

**C.4 Signing and notarisation.** *(1 day + procurement)* Without notarisation Gatekeeper
refuses the app and the bug report is "it doesn't open". **Needs a paid Apple Developer
account — a decision with a fee attached, flagged now rather than at release time.**

**C.5 Gates.** *(0.5 day)* The same three as B.4.

### MILESTONE D — the first-run shader build (~5 days, and RE-PLANNED by D.1)

**D.1 IS DONE (part 82), and it changed the shape of the rest of this milestone.**
`tools/vo_extract_microcode.py` decodes the container:

```
microcodeStart  = u32@0x04 + u32@( u32@0x18 )      big-endian, magic 0x102A110{0,1}
microcodeLength = objectLength - microcodeStart    the microcode is always the TAIL
```

423 of 423 against ground truth, and the gate — every extracted blob must FNV-1a hash to a
name already in `assets/shader_spv/`, which is exact and needs no interpretation — reads:

```
1571 objects -> 1279 pixel, 143 vertex, 149 refused (all .scv, a different container)
pixel : 343 of 345 cache entries reproduced BYTE-FOR-BYTE
vertex:   0 of 104  — and this is structural, not a decode bug
the disc also holds 922 pixel shaders NO RUN HAS EVER BOUND
```

**The two halves of the cache are now two different problems, and that is the finding.**

* **PIXEL — solved, and solved COMPLETELY.** 1,265 distinct pixel shaders on the disc against
  the 345 this project accumulated over 25 parts and eleven operator sessions. A first-run
  pass can translate all of them, which retires "the cache is complete" as a claim with a
  shelf life for the pixel half. The two the cache holds and the disc does not —
  `ps_438c2af84c78a133` (36 B) and `ps_a15c6c9c2d249375` (60 B) — are enumerated in the
  gate's threshold so it cannot pass by accident.
* **VERTEX — cannot be prebuilt at all.** §1.4's retraction: the title patches the fetch
  instructions at load from the vertex declaration, and those fields are what decide the
  vertex format XenosRecomp emits. **D.4 is therefore not a safety net, it is the PRIMARY
  path for every vertex shader**, and D.2 is a hard prerequisite for a shipped build rather
  than a nicety.

**D.2 In-process translation.** *(2 days — and it is now REQUIRED, not an optimisation)*
Link XenosRecomp (MIT, sibling checkout, already patched) and embed DXC. **Gate:** build the
cache the new way and diff the SPIR-V against the 449 modules already on disk — byte-identical
is a stronger check than any picture test, and a disagreement names the shader.
**Do this before D.3.** Without it there is no vertex shader at all, and the pixel prebuild is
a nice-to-have on top of a runtime that cannot draw.

**D.3 The first-run pass and its progress UI.** *(1 day)* Both stages of §2.3, parallel across
cores, resumable, keyed so a partial run is detected and finished rather than restarted. It
now has an honest job description: **translate the 1,265 disc pixel shaders**, and pre-warm
the pipeline cache. Vertex shaders are not part of it.

**D.4 The runtime first-sight path.** *(1 day)* On `[imload]` of a hash the cache does not
hold, translate in-process and add it. **This is where every vertex shader comes from**, plus
the two pixel shaders absent from the disc and anything the container scan mis-parses.
**Gate:** delete a shader from the cache, confirm it is rebuilt and the picture is unchanged.
The stronger gate is free and should be the standing one: start with an EMPTY vertex half and
confirm the run reaches the crowd with `no translated shader` = 0.

**~~D.1 Decode the `.vo`/`.po` container.~~ DONE** — `tools/vo_extract_microcode.py`, and the
route to it is in that file's docstring because it is the transferable part: the start offset
is not in the fixed header, it is one indirection away, and scanning for a dword with the
right value found a *spurious* perfect discriminator (the blob length — small blobs happen to
have no constant block) where dumping the structure found the real field.

**What D retires:** "the cache is complete" as a claim with a shelf life, for pixel shaders;
the six variant caches drifting apart; and the name-diff gate that exists only because of that
drift — a defect that once cost three parts when the play cache was ten modules short.

### MILESTONE E — packaging, CI and the release itself (~4 days)

**E.1 CI, honestly scoped.** *(1 day)* GitHub Actions on `ubuntu-latest`, `windows-latest`,
`macos-14` **cannot produce the release — no runner has an XEX.** What it can do is compile
the ~30 host sources on all three platforms on every push and run `--smoke` against a stub
image, which catches the overwhelming majority of what a port breaks. **Say that in the
workflow's own comment, or a green tick will later be read as "the release builds".**

**E.2 The local release script.** *(1 day)* One command per platform: configure → build →
strip → bundle → package → checksum, emitting the artifact names in §2.1 and a manifest.

**E.3 `THIRD_PARTY.md`, generated not written.** *(0.5 day)* See §4; generate the attribution
block from what is actually linked so it cannot drift.

**E.4 README and troubleshooting.** *(0.5 day)* The drop-in steps, the first-run explanation,
and the bisection knobs of §5.

**E.5 The release checklist run end to end on a clean machine.** *(1 day)* §5.

---

## §4 LICENSING — settle before the first artifact leaves the machine

Building for yourself and shipping to others are different questions, and this repo has never
had to answer the second.

| component | licence | what shipping requires |
|---|---|---|
| this repo | PolyForm Noncommercial 1.0.0 | fine — but see ffmpeg; a GPL bundle would conflict |
| XenonRecomp / XenosRecomp | MIT | attribution |
| SDL2 | zlib | attribution — and **it must be REAL SDL2, not Fedora's `sdl2-compat`** (`tools/build_sdl2.sh`); see §9.1 A.4 |
| **ffmpeg (libavcodec/libavutil)** | LGPL, **or GPL if `--enable-gpl`** | **SETTLED (part 82).** `tools/build_ffmpeg_lgpl.sh` builds LGPL, xma1+xma2 only, checked against configure's own `config.h` rather than the flags passed. Dynamically linked, notice and recipe in the generated `THIRD_PARTY.md` |
| MoltenVK | Apache 2.0 | attribution (macOS) |
| o1heap, simde | MIT | attribution |
| DXC (embedded in D.2) | Apache 2.0 with LLVM Exceptions | attribution |
| UnleashedRecomp | **GPLv3 — structural reference only, no code** | nothing to ship; keep it that way |

---

## §5 THE RELEASE CHECKLIST — every artifact, every time

1. `cz_runtime --smoke` on the packaged binary, on a clean machine with no dev packages.
   **Automated: `tools/release_gate_clean_container.sh` does 1, 2 and 3 in one podman run.**
2. `ldd` / `otool -L` / Dependency Walker: nothing resolves outside the bundle except the
   Vulkan loader and system libraries. **AND THEN RUN IT** — a `dlopen` is invisible to
   `ldd`, which is exactly how the first bundle passed this step and died on its first
   instruction (§9.1 A.4).
3. First-run flow from nothing: drop package → extract → shader build → game starts.
4. The headless crowd route reaches ≥8,000 draws; `part80_trace_band.py` puts the frame in the
   same regime as the reference build. **Cheaper first: `tools/release_text_identity.sh`.**
   If `.text` is byte-identical to the reference build, the build type cannot have changed
   the frame and the route run is a confirmation rather than the evidence.
5. `CZ_VK_VALIDATION=1`: **6 `topology-08773` and nothing else** — the standing baseline.
6. `CZ_VK_VERIFY_BIND_BATCH=1`: **0 disagreements**.
7. Shader cache: `shader_dim_census.py` clean; `rt_world_xform_census.py` exit 0;
   `vo_extract_microcode.py <objects> --gate` reproduces the pixel half from the disc.
8. **The part-81 bisection knobs are in the README's troubleshooting section**:
   `CZ_VK_NO_BIND_BATCH=1` and `CZ_VK_NO_DEVICE_PFN=1` are live by default with their
   milliseconds never measured, and a release is the first time a stranger runs them
   (`docs/part82-kickoff.md` §0). **Either price them before the first release, or document
   them as the first thing to try.**

---

## §6 RISKS, AND WHAT WOULD REFUTE EACH

| risk | severity | what settles it | when |
|---|---|---|---|
| **MoltenVK cannot host the bindless texture heap** | **high — could make macOS its own project** | C.3's three questions | before any macOS promise |
| The `.vo` container does not decode cleanly | medium | D.1's 416-of-416 gate; fallback is the search that already works | D.1 |
| Windows placeholder mapping misbehaves under a debugger or with EAF/ACG mitigations | medium | B.1's aliasing unit test, plus a run with Exploit Protection on | B.1 |
| A release build type changes the frame | low-medium | A.3's regime check | A.3 |
| ffmpeg bundling drags GPL in | low, but legally sharp | build LGPL explicitly and check `--enable-gpl` is absent | before E.2 |
| The two unpriced part-81 changes misbehave for a stranger | low | price them, or document the knobs | before release |
| Apple notarisation blocks at the last moment | low | C.4, done early, not at packaging time | C.4 |

---

## §7 OUT OF SCOPE

* **Intel Macs / x86 macOS.** The instruction is M1 and up.
* **32-bit anything; Steam Deck and handheld packaging.** Later, if ever.
* **Shipping the game DATA.** The package and everything extracted from it stay the player's
  to supply. Note that with shape (A) the recompiled image inside the executable is
  game-derived even so — stated plainly rather than left implied.
* **Performance work.** Parked since part 81. The one exception is the §5.8 checklist item,
  which is a release-safety question rather than a performance one.
* **Auto-update.** Not for a first release.

---

## §8 ORDER AND ROUGH TOTAL

```
A  shippable tree      3 d   ── required, blocks everything
B  Windows             5 d   ── independent of C
C  macOS               5 d   ── C.0 first; C.3 can force a redesign
D  first-run shaders   6 d   ── independent of B and C; the biggest item
E  packaging + CI      4 d   ── needs A; final step needs B, C, D
                      ─────
                      23 d   working days, sequential-worst-case
```

**A → (B ∥ C ∥ D) → E.** B, C and D touch disjoint files, so if any of them can run in
parallel the critical path is A + max(B, C, D) + E ≈ 13 days.

~~**Start with A.1 and A.2.**~~ **A IS COMPLETE and D.1 IS DONE (part 82, §9).** The next
item is **D.2 — in-process translation — and D.1 promoted it from a nicety to a hard
prerequisite**: the disc supplies the pixel half of the cache completely and the vertex half
not at all, so a shipped build cannot draw a single vertex shader without it. B and C remain
blocked on hardware this machine does not have (a Windows toolchain, a Mac).

---

## §9 EXECUTION RECORD — what was built, and what each gate actually measured

Filed here rather than in a separate document, because a plan whose corrections live
somewhere else gets read without them.

### 9.1 Part 82 — MILESTONE A COMPLETE, and D.1

**A.1 — `runtime/host/host_paths.{h,cpp}`.** One root, decided once and printed once
(`[paths] root … (assets-walk), exe …` on the first line of every log). `$CZ_ROOT`, else a
walk of at most four levels from the executable's directory to the first one containing
`assets`, else the executable's directory. That one rule covers both layouts: the dev tree
(`runtime/build` -> repo root, two levels) and the shipped tree (exe dir, zero). **Nothing
falls back to the CWD** — a CWD fallback keeps the dev tree working while the shipped one
silently does not, which is the single failure mode that survives every test done on the
build machine. All three platform spellings written now (`readlink`, `GetModuleFileNameW`,
`_NSGetExecutablePath`) so milestone B's file list stays the five §1.1 censused.

*Gate:* every headless recipe from `runtime/build`, the repo root, `/tmp` and `$HOME` — same
root, same 449 shader modules, `no translated shader` = 0, `--smoke` OK from all four.

**A.2 — `runtime/host/first_run.{h,cpp}`.** Package -> unpacked game -> shader cache, in
order; the first one missing prints what it is, where it goes and the command that produces
it, then exits non-zero. A missing shader cache is fatal only when `CZ_VKDRAW` is set, because
every log-diff gate in this project runs with the renderer off and refusing those on a fresh
clone would take the gates offline to protect a player from a screen they will not see.
`CZ_NO_FIRST_RUN_CHECK=1` turns it off.

*Gate — six trees, and it found two defects in the first version by running the branches on
purpose (gotcha 30):*

* a 2 MB `.wav` dropped into `assets/package/` was **accepted as the game** and the player
  told to unpack it. The magic word was reported as a footnote rather than deciding, so the
  footnote was unreachable. Size and identity are two questions now.
* the printed extract command said `-o <root>/assets`, which would have produced
  `assets/default.xex`. `extract_stfs.py`'s `-o` is the directory the package's own tree goes
  into: `assets/game`.

**A.3 — the `Release` build type, and its gate answered for free.** `-O2 -g -DNDEBUG` then
`objcopy --only-keep-debug` / `--strip-debug` / `--add-gnu-debuglink`. **-O2 and not -O3**
because the whole performance corpus behind parts 47-81 was measured at -O2 and shipping -O3
would invalidate it for a saving nobody has measured; -O3 is a measurement to run, not a
default to inherit from CMake. **-g kept then split** because the plan's "no -g, stripped,
with a separate `.debug`" is two halves in tension — a binary built without `-g` has no debug
info to keep. Verified: `addr2line` on the STRIPPED binary resolves `main` to
`runtime/main.cpp:161` through the debuglink.

*Gate — `tools/release_text_identity.sh`, and this is the transferable part.* The plan proposed
the crowd route, which on this workload is three runs an arm and an hour (gotcha 229). But
`-g` does not affect code generation and `--strip-debug` does not touch `.text`, so the claim
to test is **byte identity**, not a frame time:

```
.text  35,651,455 bytes, sha256 IDENTICAL between RelWithDebInfo and Release
```

The build type therefore **cannot** have changed the frame. Decisive where an A/B is
statistical. Positive control: exit 1 on two genuinely different binaries.

It also found something not predicted: **`CZ_BUNDLE_RPATH` moves `.text`**. It is a link
option, but the RUNPATH string lives in `.dynstr`, which sits *before* `.text`, so adding one
lengthens an earlier section and relocates the image — every address-bearing byte differs
while every instruction is the same instruction. Held matched rather than carved out as an
exception, because an exception is where a real difference would hide.

**A.4 — the bundle, and the defect only RUNNING it could find.**

`tools/build_ffmpeg_lgpl.sh`. Fedora's ffmpeg is `--enable-gpl` (a conflict with this repo's
PolyForm licence the moment they ship together) and its closure is **120 shared objects** —
x264, x265, SvtAv1Enc, librsvg, cairo, pango, OpenCL, VA-API — against the **fourteen** ffmpeg
functions this runtime calls, censused rather than assumed. Purpose-built: libavcodec +
libavutil, xma1+xma2 only, 1.4 MB, linking libm and libc and nothing else. **120 -> 3.**
Version pinned to 8.1.2, the one this machine develops against, so the headers are not a
second variable. The licence check reads configure's own `config.h`, not the flags we passed.
Verified decoding: 143,616 float samples, ch=2, 48 kHz, `fltp`, rms 0.0218.

`tools/build_sdl2.sh`, **and this is the finding worth keeping.** The first bundle passed its
`ldd` check completely — every library resolved inside the bundle — and then died on its first
instruction in a clean container with **`Failed loading SDL3 library.`** Fedora's
`libSDL2-2.0.so.0` is `sdl2-compat`, a shim that `dlopen()`s `libSDL3.so.0`. **A `dlopen` is
invisible to `ldd`, the very tool this plan's A.4 specifies for the job**, and it worked on
the build machine because SDL3 was installed there. Bundling SDL3 as well would not fix it
either: the shim dlopens by soname and has no RUNPATH. So the script builds real SDL2
(2.32.10, zlib), which links libm and libc and dlopens X11/Wayland the way every shipped Linux
game does, and asserts the result mentions `libSDL3` by **no** route.

`tools/release_package_linux.sh` assembles §2.2's layout. Libraries come from the binary's own
`ldd`, not a hand-written list that drifts. `$ORIGIN/lib` RPATH rather than a launcher script,
because a launcher is a thing a player can bypass and then the bundle silently is not what
loads. `THIRD_PARTY.md` generated from what is actually linked. **16 MB compressed.**

`tools/release_gate_clean_container.sh` is A.4's gate: podman + `fedora-minimal`, bundle
read-only, the Vulkan loader installed because that is the one library a player's driver
supplies and its absence would force an exception exactly where a real missing library could
hide. **PASSED**: every bundled dependency resolves inside the bundle *including libstdc++
with a system copy present* — which makes it a real RPATH-precedence test — `--smoke` passes
in the packaged binary, and the first-run refusal prints correctly from a container with no
game.

**The gate's own first run printed `GATE PASSED` having executed nothing**: `podman` without
`-i` does not attach stdin, so `sh -s` read EOF and exited 0. Gotcha 483's shape reproduced in
a tool written the same afternoon that gotcha was read. It now requires four marker lines and
the smoke sentence as evidence its body ran, not just an exit code.

### 9.2 What part 82 leaves OWED, stated rather than smoothed over

1. **The ffmpeg and SDL2 swaps are real `.text` changes and their cost is unmeasured.**
   Confined to the audio decode path and the window/present seam, neither of which is the
   render hot path — but that is a reason to expect a null, not a measurement of one.
2. **The shipped ffmpeg has no hand-written x86 assembly.** This machine has no `nasm` and no
   sudo. `tools/build_ffmpeg_lgpl.sh` says so loudly and falls back rather than shipping a
   slower decoder quietly. `sudo dnf install nasm` and re-run before cutting a real artifact.
3. **The artifact inherits this machine's glibc floor** (2.43) and will refuse to start on
   anything older. The packaging script prints this. The real fixes are an old build base or
   an AppImage runtime, and both are E.2.
4. **No AppImage yet** — §2.1 names one and part 82 shipped the `.tar.zst` half only.
5. **`tools/extract_stfs.py` is shipped but `build_shader_spv.sh` is not**, because it needs
   XenosRecomp and DXC. That is exactly what D.2 moves in-process; until it lands, a shipped
   build needs a shader cache supplied alongside it.
