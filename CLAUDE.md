# Dead Rising 2: Case Zero Xenon Recomp — project guide

Static recompilation of the Xbox 360 XBLA title **Dead Rising 2: Case Zero**
(Capcom / Blue Castle Games, 2010) using **XenonRecomp** + **XenosRecomp** (hedge-dev's
faithful recompiler pair, the ones UnleashedRecomp uses).

This is the **third** game ported with this pipeline in this workspace. The two before it
are the playbook, and most infrastructure and every hard-won gotcha transfers — **read
them before re-deriving anything**:

- `~/GithubRepo/Fable2XenonRecomp` — the original and the deepest (91k functions → a live
  rendered world). Its `CLAUDE.md` is the project journal; its `docs/` hold the reusable
  methodology.
- `~/GithubRepo/Asuras_Wrath_Xenon_Recomp` — the second port, which proved the template
  transfers and consolidated the gotchas into a numbered list.

**Dead Rising 2: Case West is planned next** (`~/GithubRepo/Dead_Rising_2_Case_West_Xenon_Recomp`,
package present, not started). It is the same engine and the same studio, so essentially
everything learned here should carry over — worth keeping that in mind when deciding
whether a finding belongs in a Case Zero doc or in a general one.

## Template status: what is inherited vs. what is new here

Inherited wholesale:
- Repo shape, gitignore policy (game data + generated `ppc/` untracked), tools, docs style.
- Runtime architecture: kernel HLE with *honest-failure* stubs, o1heap guest arenas,
  guest-thread bootstrap, PM4 command processor, Vulkan renderer on XenosRecomp SPIR-V,
  XMA via ffmpeg, SDL window/input. UnleashedRecomp is GPLv3 → **structural reference
  only**; guest structs from XenonRecomp's `XenonUtils/xbox.h` (MIT).
- Ground-truth discipline: Xenia text logs, `.xtr` GPU stream traces,
  `--trace_function_data` coverage, and A/B methodology with same-binary arms.

**New here, because this is the first XBLA (not disc) title in the workspace** — this is
the part a future Case West port will reuse verbatim:
- `tools/extract_stfs.py` — STFS/SVOD container reader. There is no ISO and no
  `extract-xiso` step; the game is a single hash-interleaved block filesystem.
- `tools/xex_image_dump.cpp` + `build_xex_image_dump.sh` — offline image via XenonRecomp's
  own loader, because this XEX is LZX-compressed and `decrypt_xex.py` cannot read it.
- `tools/find_save_restore.py` — structural ladder scan, replacing the hand-encoded
  byte-pattern greps the earlier ports used.
- A XenonRecomp patch for the **devkit AES key** (`docs/xenonrecomp-upstream-bugs.md`).

## Transferable gotchas

**THE FULL NUMBERED LEDGER IS `docs/gotchas.md` — 245 entries, and every "gotcha N"
reference in this repo and in the docs resolves there.** It was split out of this file
on 2026-08-08, when this file reached 308 KB and was being loaded into every session
whole. Read it **before making a measurement claim, adding an instrument, believing a
zero, or trusting a number an earlier session wrote down**; those four situations
produced almost all of it.

The ten that bite most often, as one-liners. Each is a summary, not the entry:

3.  **A zero is a detection failure, not a fact.** XenonAnalyse found zero jump tables
    here; our scanner found 234. Applies to every number a detector prints.
5.  **Kernel stubs must fail honestly, never fake success** — and a stub that returns an
    error but leaves its OUT-PARAMETER untouched is worse than no stub. See also 59 and
    201: when a return value is a predicate or a computed value, "fail honestly" has no
    spelling and implementing it is the only correct option.
7.  **A probe expensive enough to stall the game manufactures the stability it reports.**
    Every instrument needs its own control — and see 151, an arm with no counter cannot
    be shown to have engaged, and 223, an instrument on a hot path can cancel the effect
    it is measuring exactly.
13. **A capture request, a plan, and your own status note all have a shelf life.**
    Re-read them against the current ledger before believing their conclusions.
25. **A grep that cannot match is not a clean result.** Check the emitter before
    believing a zero — and 109, a capped or thinned log line is not a count.
30. **A test that has never failed has not been shown capable of failing.** Break the
    implementation on purpose and confirm the test screams. Applies to diagnostics and
    to instruments as much as to tests (94, 158).
50/51/86. **A rate measured once is a fact about that afternoon, and the control is the
    old binary run NOW** — not its remembered numbers. 159: a bimodal arm makes every
    single-run claim a coin flip.
133/127. **One frame of an animated scene is ONE SAMPLE**, and that applies to LOOKING,
    not just to measuring.
172. **A retirement is only as good as the ORACLE it was measured on.** Re-ask your own
    earlier A/Bs whenever an upstream defect is fixed.
190/103. **A gate that needs a human is a capture request in disguise** — extend the arm
    until it is not. 222 is the performance form: ask what the WORST case renders and
    whether your harness can reach it.
237/238. **A MEAN frame time measures this title's vblank pacing floor, not your change**
    — read medians and the share of frames pinned to a 16 ms multiple, which is the far
    more sensitive statistic (10% -> 97% where the mean moved 1.7%). And **a profiler
    column that falls to zero is not a saving until you find where the replacement work
    got charged**: `streams` read 0.0% while the guard that replaced it doubled `record`.

## Inherited from the Fable 2 port: shared-decode cross-checks, and a do-not-chase list

Everything below is **hardware-level decode**, not title-specific, so a defect found in
one port is a defect in the other unless this one was written differently. Checking is
minutes; diagnosing the symptom is days. **Both of the first two were checked in
session 21 and this repo is correct on both** — recorded so the next session does not
re-check them, and so Case West can check them in one grep.

| shared decode | correct form | this repo |
|---|---|---|
| fetch-constant SIZE field — the endian bits occupy the low 2 bits of `fdw1`, so `fdw1 & 0x7FFFFF` swallows them (reads ~4x too large, permits reads past the buffer, and *under*-reports past ~2^21 dwords) | `(fdw1 >> 2) & 0xFFFFFF` | ✓ `gpu/xenos.h:125`, and gotcha 110 is the same finding arrived at independently |
| `num_format_all` INTEGER semantics — a fetch declaring unsigned/integer on `k_8_8_8_8` bound as `R8G8B8A8_UNORM` delivers 0..1 where the guest asked 0..255 (typically packed bone indices, TEXCOORD-wrapped as 360 titles do) | deliver the integer as its own value into a FLOAT input | ✓ via `USCALED`/`SSCALED` in `XenosVertexFormat`, gotcha 122 |

On the second: Fable 2 fixes it by emitting `* (2^bits − 1)` at translation time and
warns *against* rebinding to a UINT format. `USCALED`/`SSCALED` is not that rebind — it
is the same semantic (an integer delivered as its own value into a float input) obtained
without touching the emitter or the vertex input signatures. Both are correct; ours
costs nothing in XenosRecomp, which matters because that recompiler is shared.

**Confirmed NON-issues — do not chase these.** Measured over ~860 shader blobs and 971
vertex fetches in the **Fable 2** bank, so the provenance is another title and the
census has not been repeated here (one pass over our 336 shaders would settle it, and
`tools/gdis.py` plus the meta sidecars are enough to do it):

- the guest requests **8-in-32 endian on 100%** of fetches;
- **`exp_adjust` is declared but zero everywhere**;
- the Xenos compiler emits a **`yxwz`-shaped destination swizzle on ~87% of 16-bit
  fetches** (identity on 32-bit) that compensates the 8-in-32 pair transposition, and
  XenosRecomp already honours it on both the declared and `XeVfetchDep` paths.

**That third one is a live lead here, not just trivia.** It is the most plausible
explanation for phase 5 §6n, where disabling this port's own 16-bit texcoord unswizzle
(`g_SwappedTexcoords`, 616,417 draws a boot) had **no measurable effect on the picture**
— if the shader already compensates via its destination swizzle, our mask is
compensating a second time or not at all. §6n recorded the null result honestly and
could not explain it; this is the explanation to test. Refutation by compensation, and
it is exactly why that rule is in the conventions.

## Layout

- `config/CaseZero.toml` — XenonRecomp main config: helper addresses, plus 139 function
  overrides from three sources that **merge, never replace** each other — switch-tail
  repairs (`tools/fix_switch_function_bounds.py`), coverage-recovered entry points
  (`tools/coverage_to_function_overrides.py`), and truncated-function widenings
  (`tools/find_dropped_branches.py --widen`). Regenerating any of them from a stale
  `ppc/` silently under-reports; always rebuild `ppc/` from the committed config first.
- `config/CaseZero_switch_tables.toml` — 232 jump tables (105 absolute, 85 offset8,
  42 offset16, 6,114 labels) from `tools/find_jumptables.py`. **XenonAnalyse finds zero
  here** — see gotcha 3.
- `assets/package/` — the XBLA STFS package as delivered (gitignored; copyrighted).
- `assets/game/` — what `tools/extract_stfs.py` unpacked out of it: `default.xex` +
  `data/` (gitignored).
- `assets/game/default_image.bin` (+ `.sections`) — the loaded image for offline
  analysis, from `tools/xex_image_dump`.
- `ppc/` — generated C++ (gitignored; 156 MB, 57,822 functions, regeneratable).
- `tools/` — analysis scripts. Several copied from the earlier ports; provenance in
  their headers. `gdis.py` is the guest disassembler and is usually the right first
  stop for any question about what the title's own code does.
  `import_call_sites.py` is the one to reach for when implementing a kernel import:
  the capture has no return values, so the guest code that consumes the result is the
  specification (finding 29).
- `docs/` — the project's memory. **Read in this order for a new session:**
  - **`xenia-capture-analysis.md`** — the numbered findings ledger, and the authority on
    any measured number: where another doc disagrees with it, it wins.
  - **`gotchas.md`** — the 245-entry transferable ledger. Every "gotcha N" resolves here.
  - **`port-history.md`** (what each session established) and **`open-items.md`** (the
    backlog, in order) — both split out of this file on 2026-08-08.
  - **`d3d-translation-plan.md`** — the renderer-architecture pivot, its recon tables and
    licensing, plus the per-phase build-out records. **The first read before any renderer
    work.** `d3d-kickoff.md` and `d3d-phase-c{,2..23}-kickoff.md` are the per-part
    hand-offs, each superseding the last; the newest is the live one. A kickoff's most
    valuable section is its list of the parts of that phase that **already exist** and
    would otherwise be rewritten from the plan text — write that section for every phase.
  - `phase1-notes.md` / `phase3-notes.md` / **`phase5-notes.md`** — the per-phase records
    of what the runtime work found that neither the plan nor the kickoff predicted, with
    `phase{1,3,5}-kickoff.md` the matching hand-offs and `runtime-plan.md` the phase plan.
  - `instruments.md` (every env var and arm), `measurement.md` (how to judge a change),
    `perf-cpu-plan.md` (the live performance plan) and `perf-plan-overnight.md` (its
    executed predecessor).
  - Formats and tooling: `big-archive-format.md` (the cracked `.big` container),
    `xtr-decoder.md` (the GPU stream format + the determinism method),
    `xenonrecomp-upstream-bugs.md` (local recompiler patches),
    `bootstrap-2026-08-04.md` (day 1), `xenia-capture-requests.md` (unfulfilled
    requests), `reusability.md` (what transfers to Case West), `phase5-3d-plan.md`
    (superseded, but its Step 0 instrument and Step 1 findings survive).
- `Xenia logs/` — captures land here (gitignored); keep an index in
  `Xenia logs/Xenia_Run_Content.md`, which **is** tracked.
- `~/DR2CZ-troubleshooting/` — **outside the repo on purpose**: operator screenshots
  (the only evidence channel for "does it look right", and the one no instrument here
  can replace) and headless `CZ_VK_FRAME_DUMP` frames. Its `INDEX.md` says what every
  shot showed, INCLUDING the ones that were lost — Spectacle deletes its temp directory
  when the window closes, so most of the first gameplay session's screenshots survive
  only as descriptions. Save straight into it.
- `runtime/` — the host runtime. Phases 1 and 3 complete; **phase 4's command
  processor is live too, ahead of the plan's ordering** — do not read the plan's phase
  numbers as the state of the code. There is a window, a present seam and real input;
  there is **no renderer** (phase 5), so the window is blank on purpose. Target is
  `cz_runtime`; the phase 0.2 link gate survives as `cz_runtime --smoke`.
  - `CMakeLists.txt` — **selects clang++ before `project()` (gotcha 31)**, and
    enables C for exactly one file (o1heap) so the .c source is not silently ignored.
  - `main.cpp` — image load → header publish → data-import resolution → guest entry,
    plus the `--smoke` gate.
  - `cpu/timebase.{h,cpp}` — the 49.875 MHz guest timebase, force-included over
    `ppc/` only (gotchas 1 and 32). `kernel/imports.cpp` shares `CZ_TIMEBASE_HZ` so
    `KeQueryPerformanceFrequency` and `mftb` cannot drift apart.
  - `cpu/guest_thread.{h,cpp}` — PCR/TLS/TEB block + guest stack. Both constants are
    from this XEX's header as A1 prints it: 64 TLS slots, 0x40000 stack.
  - `kernel/{memory,heap}.*` — the flat 4 GB map and the four arenas; the layout is
    checked against A1's own allocations, not inherited (`docs/phase1-notes.md` §3).
  - `kernel/{kobject,guestcall,klog}.*` — handles, the import marshalling seam, and
    the `[kcall]` trace the gate diffs.
  - `kernel/xex_imports.*` — publishes the XEX headers into guest memory and resolves
    the 244 IAT slots + 13 kernel variables.
  - `kernel/imports.cpp` — the kernel HLE, written in A1's call order.
  - `kernel/audio.{h,cpp}` — the XAudio render-driver client (a guest-thread pump
    calling the title's callback at 5.333 ms/frame) and the XMA context array +
    its MMIO register file. Finding 36; every structural claim is quoted from the
    guest function that states it.
  - `kernel/content.{h,cpp}` — the save-data layer: the content enumerators, the
    XAM enumerate message (app 0xFE, message 0x0002000E) and the mount that makes
    `save:` a host directory. Its header comment is the derivation of the whole
    protocol out of the title's own statically-linked `XamEnumerate` — read it before
    changing anything here, and lift it for Case West (gotchas 104-106).
  - `kernel/{vfs,file_imports}.*` — the file layer. In phase 1 rather than phase 2
    because A1's 22nd distinct kernel call is already an `NtCreateFile` (finding 16).
  - `kernel/import_stubs.cpp` — generated; honest-failure returns, not aborts.
  - `cpu/crash_report.cpp` — the guest state on any fault. Its host pc is the one
    field that is never stale (gotcha 57); `addr2line` it.
  - `cpu/guest_probe.cpp` — argument probes on named guest functions via the alias
    seam, behind `CZ_ARG_PROBE`. Kept as the worked example of tracing a bad value
    back to its producer; it is what closed finding 27.
  - `gpu/vk_renderer.{h,cpp}` + `gpu/xenos.h` — **phase 5: the renderer.** Inert
    unless `CZ_VKDRAW=1`. `xenos.h` holds the register indices and format codes with
    each field layout written next to it, because every one of them is a magic number
    whose wrong value is silent. The header comment of `vk_renderer.cpp` transcribes
    the interface the translated shaders present (push constants, the five descriptor
    spaces, the shared-constants offsets) out of the generated HLSL — read that, not
    this, if the two ever disagree.
  - `host/window.{h,cpp}` — phase 3: the SDL window, the event loop, the present
    seam and the pad, deliberately in **one** module because in SDL they are one
    thread. Everything except `Host_Present` (called from the PM4 executor) and
    `Host_PadState` (called from whichever guest thread polls `XamInputGetState`)
    runs on the thread that created the window, which is the process's main thread —
    which is why `main.cpp` now runs the guest entry on a spawned thread
    (gotcha 99). Compiles to honest stubs without `CZ_HAVE_SDL`.
- Recompiler TOOL at `~/GithubRepo/XenonRecomp` (built at `build/`; carries local
  patches — see `docs/xenonrecomp-upstream-bugs.md`). Shader translator at
  `~/GithubRepo/XenosRecomp` (also patched; Case Zero inherits those fixes for free).

## Commands

Unpack the game (once):
```
python3 tools/extract_stfs.py "assets/package/58410A8D/000D0000/<hash>" -o assets/game
./tools/build_xex_image_dump.sh
./tools/xex_image_dump assets/game/default.xex assets/game/default_image.bin
```

Regenerate the recompiled C++ (from repo root; `ppc/` must exist or XenonRecomp
segfaults in `fwrite`):
```
mkdir -p ppc && cd config && ~/GithubRepo/XenonRecomp/build/XenonRecomp/XenonRecomp \
    CaseZero.toml ~/GithubRepo/XenonRecomp/XenonUtils/ppc_context.h
```

Regenerate the switch tables — **use our scanner, not XenonAnalyse**:
```
python3 tools/find_jumptables.py assets/game/default_image.bin \
    -o config/CaseZero_switch_tables.toml
```

Repair function bounds after any switch-table change, then re-run the recompiler and
confirm the log has zero `jump outside function` lines:
```
python3 tools/fix_switch_function_bounds.py --apply
```

Check for silently dropped direct branches — **this is not optional after any change to
the function list**, and it is the only thing that catches the coverage oracle's
loop-header splits (gotcha 28). Regenerate `ppc/` between each step:
```
python3 tools/find_dropped_branches.py            # report both classes
python3 tools/find_dropped_branches.py --prune    # backward: remove spurious starts
python3 tools/find_dropped_branches.py --widen    # forward: widen truncated functions
```

Then check that every switch-shaped `bctr` was actually lowered — the gate for the
defect class that leaks a callee's non-volatiles into its caller (gotchas 53-55).
**Exit 1 = a real defect; run it last, and after any config change:**
```
python3 tools/find_unlowered_switches.py          # 0 defects expected
python3 tools/find_unlowered_switches.py --all    # also list the benign tail-call thunks
```

Build the SPIR-V shader cache. **`assets/shader_spv/` is gitignored, so a fresh clone
needs this before `CZ_VKDRAW=1` does anything.** Two sources, and they merge: the
captures' shaders (which reach gameplay, where our runtime cannot yet go) and our own
dump (which is the authority on the byte range, because the cache key is a hash of
it — gotcha 115). **Our dump run must go as DEEP as the runtime can go, not just to the
title screen** — the plain boot ends at the title and the prologue loads a shader
neither capture contains, which the renderer then declines to draw with (28,718 draws a
run, one line in the log and nothing else):
```
python3 tools/xenia_ucode_to_cache.py \
    "Xenia logs/A1_boot_title_fullgame/shaders" \
    "Xenia logs/A2_gameplay_stillcreek/shaders" /tmp/ucode      # 335 distinct
(cd runtime/build && CZ_NO_WINDOW=1 CZ_SHADER_DUMP=/tmp/ucode \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A timeout 300 ./cz_runtime)  # +2
tools/build_shader_spv.sh /tmp/ucode assets/shader_spv          # 337, zero failures
```
The check that costs nothing, and the only thing that reports this at all — a shader the
cache lacks is one log line and a silent counter, not a failure:
```
grep -c "no translated shader" run.log         # must be 0
```
And the gate on the sidecars themselves, which is **two-sided by construction** — the
per-slot texture dimension is derivable both from our ucode parse and from DXC's
`OpDecorate ... DescriptorSet` words, so a disagreement means one of the two decodes is
wrong. Run it after any cache rebuild; exit 1 is a real defect:
```
python3 tools/shader_dim_census.py             # 298 modules 2D, 92 cube, 0 disagreements
```
It also names the sidecars carrying no `tfetchDims` at all — cache entries built before
part 25 whose microcode is gone. **Keep ucode dumps in `~/DR2CZ-troubleshooting/ucode-dumps`,
not in `/tmp`**, which is a tmpfs: eleven entries are unrecoverable for exactly that reason.
**The cache is 397 and it has grown on EVERY session that reached new ground.** 335 from
the captures, 337 with our own dump, then 339, 353, 370, 371, 391, 394, 397 — 23 of
those from two operator play sessions on 2026-08-08 alone, once the whole-frame black
stopped hiding the parts of the map nobody had visited. **Treat "the cache is complete"
as a claim with a shelf life** (gotcha 13): every era of this game that no run has
entered is a shader gap nobody has counted, and the counter is one log line.
A1 stops at the title screen,
A2 is gameplay, and the prologue and Still Creek each loaded a shader neither capture
nor our own dump contained. Any run that reaches new ground should carry
`CZ_SHADER_DUMP` so the blobs are captured for free — including an OPERATOR run, which
is the only way this port reaches most of the game.

**And if a run finds a missing shader without `CZ_SHADER_DUMP` set, the blobs are not
lost — recover them from the LIVE process.** `[imload] VS va=%08X hash=%016llx size=%u`
prints the guest address and the dword count, `runtime: guest memory at 0x...` prints
the host base, and the renderer's own hash is a self-check on the result:
```
gdb -p <pid> -batch -ex "dump binary memory vs_<hash>.ucode <base+va> <base+va+size*4>"
python3 -c "..."   # FNV-1a over the bytes must equal <hash>
tools/build_shader_spv.sh <dir> assets/shader_spv
```
Both of Still Creek's were recovered this way and both hashed EXACTLY, which is what
makes it a measurement rather than a hopeful memory read — a stale or reused buffer
would fail the hash rather than produce a plausible wrong shader. **Part 19 recovered
TWENTY this way from a single operator session, all twenty hashing exactly**, which
takes this from a trick that worked once to the standard recovery.

**Use `process_vm_readv`, not `gdb`, when the operator is still playing.** It reads
another process's memory without ptrace-stopping it, where a `gdb` attach freezes the
game for a second — which during a load test contaminates the very thing being tested.
Same permission model, no interruption; ~30 lines of `ctypes` and the FNV-1a check is
identical.

Build the runtime (needs `clang++`, **SDL2 and Vulkan**; ~90 s on 16 cores for a cold image
build). SDL2 is required rather than optional-with-a-fallback, because a build that
silently lost its window would look exactly like a run whose input stopped working;
`-DCZ_WINDOW=OFF` is how you say "headless on purpose" out loud. Vulkan is required for
the same reason and is safe to require, because the renderer is off at RUN time unless
`CZ_VKDRAW=1`:
```
python3 tools/gen_import_stubs.py                 # after any change to the import set
cmake -S runtime -B runtime/build -G Ninja
cmake --build runtime/build -j$(nproc)
./runtime/build/cz_runtime --smoke                # the phase 0.2 link gate, still live
```

Reach live GAMEPLAY headlessly. Until part 16 this needed an operator, so every
gameplay claim was a report with no reproduction (gotcha 190). **START skips a
cinematic**, and the Zombrex tutorial's second page needs D-pad LEFT to open the watch
and B to leave it — without those two the run parks on the card forever:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,NONE \
  CZ_VK_FRAME_STATS=/tmp/gp.txt timeout 330 ./cz_runtime > /tmp/gp.log 2>&1)
tail -200 /tmp/gp.txt | awk '{print $5}' | sort -u | wc -l     # 200 = the camera moves
```
Arrives at ~185 s and reaches file **#184**, ~1,860 draws a frame. **Check the
camera-distinctness number before trusting anything measured off it**: every step is a
fixed 8 s interval against a boot whose depth in fixed wall time has always been a
distribution (gotcha 75), so the press counts will drift with load or frame rate. It
MANUFACTURES progress, so it is never a gate configuration (gotcha 78).

Reach the OUTDOOR WORLD and a CROWD headlessly — Chuck walks out of the safehouse into
Still Creek and the camera sweeps. **This is the recipe every gameplay-rendering and
gameplay-performance question needs**, because the one above parks in the safehouse at
~1,900 draws, which is against the title's own two-vblank cap where a CPU saving
measures as exactly zero (`docs/perf-cpu-plan.md` item 0, which this closes). The extra
`B` is the safehouse door; a stick entry HOLDS for its whole interval where a button
taps for 150 ms, which is why walking needs `LSUP` and not `A`:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,LSUP,LSUP,B,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,LSUP,LSUP,LSUP,RSRIGHT,RSRIGHT,LSUP,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,LSUP,LSUP,RSLEFT,RSLEFT,RSLEFT,RSLEFT,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,NONE \
  CZ_VK_FRAME_STATS=/tmp/out.txt timeout 620 ./cz_runtime > /tmp/out.log 2>&1)
awk 'NR>1 && $2>m {m=$2} END {print m}' /tmp/out.txt      # >= 6,000 = it got there
```
Reaches the junkyard behind the safehouse by ~300 s at **6,400-8,100 draws a frame**,
past the operator's 6,592. Same warning as above and more so: it is 57 fixed 8 s steps
against a drifting boot, so **check the draw count before trusting anything measured off
it** rather than assuming the run went where the last one did.

Run the guest and gate it against hardware. **Both captures, always** — A1 is the
authority for the boot sequence, A5 for the synchronisation surface, and A5 is *not* a
superset of A1 (gotcha 45):
```
(cd runtime/build && ./cz_runtime > /tmp/run.log 2>&1)      # ^C or timeout; it parks
python3 tools/kernel_call_diff.py \
    --xenia "Xenia logs/A1_boot_title_fullgame/cz_run1.log" --ours /tmp/run.log
python3 tools/kernel_call_diff.py \
    --xenia "Xenia logs/A5_highfreq_boot/cz_run5.log" --ours /tmp/run.log \
    --include-high-frequency
```

Runtime instruments and measurement arms: **the full catalogue is
`docs/instruments.md`** — every environment variable this runtime reads, what it
measures, and for the ARMS what they are the control for. All are off by default and
free when off. Split out of this file on 2026-08-08.

The ones reached for most often:

```
CZ_NO_WINDOW=1     headless; the control arm for every phase 3 claim
CZ_VKDRAW=1        the renderer. OFF by default, so the same binary is the control arm
CZ_VK_PROFILE=N    the frame's CPU time by phase, every N seconds, plus a `pump` line
                   splitting the graphics pump's own ticks/sleep/walk/`pm4`, plus a
                   packet census (packets/frame, ns/packet, register dwords/frame).
                   START HERE for any performance question — and read the GPU's clock
                   before believing the `submit` column (gotcha 219). **Every phase is
                   EXCLUSIVE of the ones nested inside it as of part 20; it was not
                   before, and numbers from earlier sessions overstate `record` by the
                   whole of `streams` (gotcha 228)**
CZ_VK_FRAME_STATS=file   one line per presented frame; input to tools/frame_compare.py
CZ_VK_FRAME_DUMP=dir     every 64th frame as a PPM — the picture, self-servable
CZ_VK_SNAP_DUMP=dir      EVERY resolve snapshot of one frame: which PASS went wrong
CZ_RING_TRACE=1    the ring, the brake's health, and the GPU/CPU hand-off chain counted
                   link by link. `truncated=0` is a standing gate
CZ_FILE_TRACE=1    every open/read, including the not-founds
CZ_WAIT_TRACE=1    name any infinite wait that outlasts 5 s, with guest callers
CZ_FAKE_PRESS_SEQ=...    synthetic input. MANUFACTURES PROGRESS — never a gate run
CZ_GUEST_LOG=1     the engine's OWN debug printf (640 callers, gated off in a shipped
                   build — raising those gates is open work, gotcha 215)
CZ_SHADER_DUMP=dir put this on any run that might reach new ground, including an
                   operator run: a missing shader is one log line and a silent counter
CZ_DEBUG_MENU=1    enables retained debug scaffolding; at the title menu press F2 to
                   open the shipped, operator-confirmed DebugJump testing screen; F4
                   opens the host-rendered Case Zero debug submenus (Left goes back)

```


`CZ_KCALL_WHO` is the companion to the phase gate: the gate says *that* our
first-occurrence order diverges, and the most informative divergences are imports we
call which hardware never calls at all. Only the call site explains those.

**The measurement and analysis recipes live in `docs/measurement.md`** — the renderer
A/B and its noise floor, the picture checks against capture E (`frame_signature.py`,
`frame_sharpness.py`), the guest disassembler `gdis.py`, the `.xtr` capture tools, the
bin-predication oracle and the two PM4 boundary oracles. **Run the PM4 oracles after any
command-processor change** (exit 1 = we would desync on a real stream), and never claim a
renderer improvement without a same-binary A/B aggregated over an era.

## The recompilation contract (identical to Fable 2 and Asura's Wrath)

- Every guest function → `PPC_FUNC_IMPL(__imp__sub_XXXXXXXX)` taking
  `(PPCContext& ctx, uint8_t* base)`.
- Guest 32-bit addresses index into `base`; `PPC_LOAD/STORE_*` swap endianness.
- Hooks: define a strong `PPC_FUNC(sub_X)` calling `__imp__sub_X(ctx, base)` pre/post.

## Game intel (established 2026-08-04)

- **Package**: XContent `LIVE`, content type `0x000D0000` (Arcade Title), STFS volume,
  256 files, 825 MB. Display name `DEAD RISING 2: CASE ZERO`. Title ID `58410A8D`.
- **XEX**: image base `0x82000000`, entry `0x825D9F30`, image size `0xB40000`,
  `.text 0x82150000 + 0x873564`. Encryption 1 (**devkit key**), compression 2 (**LZX**).
  244 imports.
- **Engine**: Blue Castle Games' in-house engine, shared with the full Dead Rising 2 —
  the image still carries DR2's zone names (`americana`, `atlantica`, `arena_stadium`,
  `boss_battle_*`) though Case Zero ships only the Still Creek content. This is why Case
  West should be cheap after this.
- **Middleware**: Havok physics (`hkp*`/`hkx*` RTTI), XMA audio, an in-house
  "CrowdEngine" for the zombie crowds. ~~Bink video.~~ **Retracted (finding 7)** — the
  image contains the strings `Bink_1`/`Bink_2`, which is what the day-1 inference was
  built on, but there is no Bink decoder in use and no `.bik` file in the package. The
  strings are a dead or renamed path. Middleware named in an image is evidence that a
  name exists, not that a codec runs.
- **Assets**: `.big` archive containers throughout, `.bct` textures, `.bcf` fonts. At
  least one path is constructed at runtime (`anm_%s.big`), so the VFS must handle
  arbitrary paths rather than a fixed manifest. Format not yet cracked; Fable 2's `.bnk`
  work is the closest model.
- **Shaders ship loose on disc**:
  `data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big`. ~~If those hold raw Xenos
  microcode they feed XenosRecomp almost directly.~~ **Retracted (finding 6):** they are
  `.big` archives of `<hash>.vo` shader *objects* carrying build metadata (including
  `.updb` debug paths), and their payloads share only background-noise n-gram overlap
  with the microcode the guest actually submits. The renderer input instead comes from
  Xenia's `dump_shaders`: 455 microcode blob files = **335 distinct shaders** (A1's
  120 are a strict subset of A2's 335), all translated in phase 5.
- **No Bink** (finding 7). Movies stream through an in-house "Movie Player Object"
  reading `.big` cinematic archives. Grep `.big`, never `.bik`.

## Ground truth in hand (round 1, delivered 2026-08-04 — COMPLETE)

**START HERE: `docs/xenia-capture-analysis.md`** — the numbered findings ledger. It is the
authority on measured numbers; where any other doc disagrees with it, it wins.

Index of what each capture is: `Xenia logs/Xenia_Run_Content.md` (written by the
operator). All runs are the **full game** (`license_mask = 1`) on the instrumented
canary fork, STFS package launched directly.

- **A1** boot→title at L3 (13.9 MB) + the Section D shader dump · **A2** gameplay
  (606 MB) + gameplay shaders · **A3** save round-trip + the physical save file ·
  **A4** 5-min title idle · **A5** A1's drive with high-frequency logging (231 MB).
- **B1/B1b** GPU `.xtr` boot→title + determinism repeat · **B2** GPU gameplay
  (**7.95 GiB** — the operator fixed Xenia's 2 GiB `.xtr` cliff at source to get it).
  Each with a same-run L3 correlation log.
- **C1/C2** function coverage: 12,278 boot→title, 17,118 gameplay, +4,840 gameplay-only.
- **E** five screenshots as the visual target.

**THERE IS NO OUTSTANDING CAPTURE REQUEST.** The only candidate is an optional A2b
(gameplay-era `.big` seek order), and finding 8 explains why it is probably unnecessary.

Highlights that change how we work:
- **The trial trap fired.** `license_mask` defaults to 0 → the game boots its **trial**,
  whose boot differs measurably (`chuckwalkietalkie.big` 1,164× vs 2×). Finding 1.
- **`NtReadFile` is `kHighFrequency`** — invisible at plain L3. **A5 is the `.big` read
  oracle**, not A1 or A2. Finding 2.
- **The `.big` container format is cracked** — `docs/big-archive-format.md`. Should
  transfer verbatim to Case West.
- **There is no Bink in this game.** Movies stream through an in-house player reading
  `.big` cinematic archives. Finding 7; the Bink phase is deleted from the runtime plan.
- **The disc shader banks are NOT usable microcode** (retraction, finding 6) — but
  Xenia's `dump_shaders` gave us 455 microcode blob files — **335 distinct** shaders,
  A1's 120 being a strict subset of A2's — which is XenosRecomp's input, so the
  renderer was unblocked anyway.

## Current status

**The full per-session trail is `docs/port-history.md`; the open backlog is
`docs/open-items.md`.** Both were split out of this file on 2026-08-08. The
authoritative per-subject records are `docs/xenia-capture-analysis.md` (the numbered
findings ledger — it wins on any measured number), `docs/phase1-notes.md`,
`docs/phase3-notes.md`, `docs/phase5-notes.md` and `docs/d3d-translation-plan.md`.

Where the port is, as of 2026-08-08 (phase C part 21):

* **The recompilation is clean and has been since phase 0**: 57,808 functions, 228 TUs,
  zero unrecognized instructions, zero dropped branches, zero unlowered switch
  dispatches, silent recompiler.
* **The game boots, renders and PLAYS.** A headless run reaches the title screen, the
  menus, the prologue and live gameplay with no operator (the recipe is in Commands).
  The operator has played Still Creek: combo weapons, the Zombrex tutorial, cinematics.
* **Gates, both arms:** `--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**;
  `truncated=0`; `no translated shader` = 0; deepest file on a no-input boot
  **#83 `cinezombie.big`**;
  both PM4 capture oracles clean; the picture matches capture E2 at **+0.9597**
  correlation, identity orientation. **A1's strict-prefix gate is BIMODAL** — the benign
  position-71 three-name interleave of gotcha 221 shows on some runs and not others, and
  `CZ_PM4_TICK_MS=16 CZ_VBLANK_TICKCOUNT=1` reduces it rather than removing it (part 18
  said "restores" on the strength of one run). Quote A5.
* **Performance: ordinary gameplay is 31 fps and CLOSED** — that is the title's own
  two-vblank pacing and it will not go higher. **Crowds are the open item and are
  CPU-bound in our runtime**: 75% of a crowd frame is the renderer's draw path and the
  PM4 walk. `docs/perf-cpu-plan.md` is the plan; item 0 is closed (the headless recipe
  reaches the outdoor world at 6,400-8,700 draws a frame) and **part 20 re-measured the
  rest, because the profiler was counting nested phases twice and the plan's ranking of
  §1 was built on the result** (gotcha 228). Corrected, at ~6,800 draws: draw path
  19.9 ms (`record` 6.7, `other` 5.6, `streams` 3.7, `textures` 2.7, `constants` 1.3),
  PM4 walk 11.8 ms. Taking the instrumentation off the per-draw path is **−11.0% of a
  crowd frame**, three runs an arm with no overlap. **The PM4 walk is a register-write
  loop — 90,316 packets a frame carrying 815,020 register dwords at 15.3 ns each** — and
  it is the biggest untouched term.
  **The stream cache is CLOSED as of part 22** — the cross-frame store (§6av) takes
  `streams` from 11.1% of a crowd frame to **0.0%**, copied bytes from 61-66 MB/frame to
  0.23, for a net 3.3-4.0 ms after its content guard.
  **The noise floor here is 10-13% at one run a side** (gotcha 229): use
  `tools/frame_perf_bins.py`, three runs an arm, alternated, and run the null comparison
  first. A real A/B on this workload is an hour of wall time.
  **AND `frame_perf_bins.py` REPORTS MEANS, WHICH ON THIS TITLE MEASURE THE PACING FLOOR
  RATHER THAN YOUR CHANGE (gotcha 237).** It scored the store at +1.7% against a +1.3%
  null; read as medians the same data is **44 ms -> 32 ms at ~3,700 draws**, and the
  decisive statistic is neither — it is the share of frames within 1 ms of a 16 ms
  multiple, which goes **10% -> 97%**. A CPU saving converts to frame rate only where the
  frame is above one vblank floor and within reach of the next, so quote the pinned share.
  **DO NOT PIN THE GPU CLOCK — gotcha 219 is retracted in part.** The P8/210 MHz this
  project quoted for five sessions was an overnight session with the MONITOR ASLEEP.
  Awake, this workload governs itself to **P5, mean 524 MHz, 32% utilisation, 28.6 W**,
  which is where `vkcube` sits on the same machine. Sample and quote with
  `tools/gpu_clock_sample.py`. **The 32% is the real finding**: the GPU is idle 68% of
  every frame because `SubmitAndWait` blocks straight after submitting, so our CPU and
  GPU never overlap. Overlapping them is ~22 -> 36 fps at the SAME power and is now the
  largest item in the plan (gotcha 231, `docs/phase5-notes.md` §6ar).
* **The view-dependent whole-frame black is SOLVED** and was the renderer's per-frame
  bump arena overflowing at a fixed 128 MB against a 161 MB peak, which lost the whole
  post chain and presented black over a correctly rendered scene. 160 black frames of
  8,216 at 128 MB, zero at 512, and every one of the 160 the frame after an exhaustion.
  It grows now. `docs/phase5-notes.md` §6ap.
* **SAVE AND LOAD BOTH WORK — a closed round trip, and the first title state in this
  port that survives a process exit** (`docs/phase3-notes.md` finding 52). The file is
  303,104 bytes with bytes 4..31 identical to the real 360 save A3 shipped. Three
  defects, each of which alone was enough: `NtCreateFile` honoured no disposition,
  `NtWriteFile` was a stub, and the VFS cached negative lookups so a created file could
  never be re-opened. The LOAD needed a fourth — xam ordinal `0x271`,
  `XamContentCreateInternal`, which `kResolvable` refused because A1's list of resolves
  was captured with an empty save root.
* **CUBE MAPS ARE BOUND AS OF PART 25** — found in part 23, built in part 25. 92 of the
  cache's 397 shaders sample set 2 (`TextureCube[]`) and every one of them read descriptor
  index 0, the 1x1 white dummy, on every draw from phase 5 until now. The sidecar now
  carries each fetch slot's dimension, cube maps upload as six faces into a
  `VK_IMAGE_VIEW_TYPE_CUBE` view in set 2, and `CZ_VK_NO_CUBE=1` is the same-binary
  control arm. **The fetch constant's own dimension field was located by CENSUS
  (`CZ_VK_DIM_CENSUS=1`), not from memory, which had it one field off** — dword5 bits
  9..10, cross-checked against dword2's stack depth reading 5 for every cube fetch
  (gotcha 244). `docs/open-items.md` item 00 and `phase5-notes.md` §6ay; three competing
  theories died in part 23's census and are recorded there so they are not re-bought.
  **What is still owed is the operator's verdict on the picture**, because the surfaces
  this should change are ones only they have named as wrong.
* **Ordinary gameplay is ~30 fps and the CPU/GPU now OVERLAP** (part 23): the fence wait
  fell from 31.5% of a crowd frame to 0.2% with `CZ_VK_FRAMES_IN_FLIGHT=2` (default; `=1`
  is the old renderer, same binary). The binned frame-time A/B is still owed.
* **THE HUD / AMMO DEFECT IS FIXED** (part 24, open item 00c). It was our own
  cross-frame stream store: its guard was exact only to 512 bytes and sampled 8x64
  above that, so a small edit inside a batched multi-KB UI vertex buffer hashed
  identical and the store served the previous frame's numbers. The exact bound is now
  16 KB (`CZ_VK_STREAM_GUARD_BYTES=N`), the HUD is correct in a gas-station crowd, and
  it costs **zero frame rate** — 32.2 ms at 6,778 draws, still the two-vblank floor.
  Gotchas 242 and 243 are the transferable halves.
* **The renderer's remaining picture defects** are the shadow cascade, NPC part meshes,
  mipmaps and the colour-grading LUT — all in `docs/open-items.md` with the measurement
  for each, and all worth re-testing now that a frame losing its post chain is no longer
  contaminating the evidence.

**The five-tool recompilation pipeline must run in this order**, re-running the
recompiler between each, because each one's evidence is only valid against a current
`ppc/`:

```
find_jumptables.py  ->  coverage_to_function_overrides.py  ->
    fix_switch_function_bounds.py --apply  ->  find_dropped_branches.py --prune / --widen
    ->  find_unlowered_switches.py
```

The last is a **gate, not a repair** — exit 1 means a real defect (gotcha 53).

## Reusability: what gets extracted, and when

**`docs/reusability.md`** — the tier list for what transfers to Case West and what does
not, and the two rules governing it: **extract only what is proven in BOTH ports, never
from Case Zero alone**, and **extract after the second implementation forces the seam, not
in anticipation of a third**. Do not build the Case West abstraction before Case West
starts.

## Conventions (same as the two template ports)

- No copyright/license headers in new files (user's own repo — ask before adding any).
- **Commit proactively** — whenever a change is useful on its own or important
  information was learned. End commit messages with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Document everything** in `docs/` for an outside reader — findings, dead ends,
  formats, retractions. Write it so someone porting a *different* Xbox 360 game can lift
  the technique: say what the idiom or format was, not just what we changed. That has
  concrete value here, because Case West is next and will read these documents.
- **Comment code for humans.** Every tool opens with a docstring answering *why it
  exists* — what went wrong without it — not just what it does. Inline comments explain
  the non-obvious bit-twiddling, the reason a scan runs forward instead of backward, and
  every deliberate exclusion. Generated config files carry a header saying which tool
  produced them and how to regenerate.
- **Retract in place.** When a stated finding turns out to be an artifact, say so where it
  was claimed and explain the artifact.
- Measurement discipline from day one: A/B with same-binary arms, gate comparisons,
  pre-register capture questions.

### Evidence rules (non-negotiable)

- **Measure before inferring.** A hypothesis about guest behaviour is tested against a
  census over the image, the shader bank or the capture — never argued from
  documentation or from model knowledge. **Report counts, not impressions.**
- **One change per experiment.** Fixes with distinct predictions land in separate
  commits and are verified separately.
- **State the prediction before running it.** Every fix commit records the falsifiable
  claim it makes about on-screen behaviour or dumped state, so a run can refute it.
- **A/B ADMISSIBILITY.** Two configurations are comparable at matched indices only if
  they are two states of ONE renderer producing the SAME draw set. If one arm renders
  less — or more — the comparison is inadmissible; say so and fall back to within-run
  evidence. This is the rule that would have caught `CZ_VK_FORCE_COLORMASK` in phase C
  part 9: the arm adds draws, so its picture cannot be compared with the baseline's at
  all, and it took a counter to settle what a picture never could.
- **Refutation by compensation beats refutation by absence.** When a mechanism is real
  but compensated somewhere else, record BOTH — that closes the branch properly, where
  "we looked and saw nothing" leaves it open. Phase 5 §6n is the worked example.
- **An untrusted path is not an oracle.** Only diff against a subsystem that has itself
  been validated, and re-ask that question whenever an upstream defect is fixed
  (gotcha 172). Case Zero has no in-project second implementation to diff against, so
  the substitutes are (a) Xenia traces, read for the FIRST divergent operation rather
  than the visibly broken object twenty frames later, and (b) the Fable 2 port for
  anything in the shared decode layer.

### Things not to do

- **Do not extract a library from this port alone**, and do not build the Case West
  abstraction before Case West starts. No interfaces or library splits "for later".
- **Do not bundle independent fixes into one commit.**
- **Do not treat documentation or prior model output as ground truth over a census** —
  including this file. Every number here was measured once and has a shelf life
  (gotcha 13).
- **Do not add speculative Xenos coverage.** An unsupported packet, format or import
  fails LOUDLY with its identifier; it never guesses (gotcha 5).
- **Do not copy external code before its licence is recorded.**
- **Do not delete the PM4 command processor.** See the note below — it is the control
  arm, not legacy.

### A conflict with the external "Project Constitution", recorded so it is not re-litigated

An outside constitution document describes this port as *"API-level HLE — a D3D
translation layer. There is no command processor in this project and none will be
added."* **That is not this repository.** `runtime/gpu/pm4.cpp` is a live PM4 command
processor: it is the boot engine (phase C's D3D arm still needs it for the ring and the
GPU/CPU hand-off), and more importantly it is the **same-binary control arm** for every
claim the D3D arm makes — the discipline the last nine sessions are built on. Phase C
part 9's four renderer fixes were all found on the PM4 arm because it is faster and
better instrumented. The constitution's *intent* — the D3D translation layer is where
the port is going — is right and is exactly `docs/d3d-translation-plan.md`; its
statement about the CP is wrong about the code and would, if acted on, delete the
ability to A/B.

Two more of its claims are superseded by measurement in this repo and should not be
re-derived: this title's tiles are **left/right 640-wide halves**, not horizontal bands
(window scissors `0..640` and `640..1280`, window offset `-640`), and its
"force a single tile, ignore predication" diagnostic HAS been run — it is
`CZ_PM4_NO_PREDICATION=1`, and it is destructive rather than diagnostic (phase5-notes
§6v).
