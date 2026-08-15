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

**THE FULL NUMBERED LEDGER IS `docs/gotchas.md` — 305 entries, and every "gotcha N"
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
172/268. **A retirement is only as good as the ORACLE it was measured on** — and YOUR OWN
    STUB IS AN ORACLE. Re-ask your earlier A/Bs whenever an upstream defect is fixed. A
    three-configuration same-binary arm retired "the prologue waits on audio" in part 16;
    it was true, and a real decoder proved it in phase A/V.
267. **A guest structure handed to a DMA device holds PHYSICAL addresses**, and in a flat
    recompiler map those are not the ones the CPU uses. Cost this port its whole audio
    subsystem for 28 parts. Print the DESTINATION on every file-IO trace.
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

**That third one was a live lead here and PART 37 CONFIRMED IT — it was the whole
striped-material class (item 0s).** The shader's destination swizzle IS the complete
correction: our `CopySwapped` leaves 16-bit pairs in exactly the state the real fetch
pipe hands the shader, so `g_SwappedTexcoords` was compensating A SECOND TIME, and
lightmap UVs (16_16 TEXCOORD2 fetches) arrived transposed — baked prop shadows painted
as hard-edged black blotches on the tanker, Dick's far LOD and the pawnshop boards.
§6n's frame-wide null was honest and blind: the damage is localized to lightmapped
props. The mask now defaults to ZERO; `CZ_VK_TEXCOORD_SWAP=1` is the control arm.
`docs/phase5-notes.md` §6bo, gotchas 291-292. For Case West: publish NO texcoord swap
mask; trust the microcode's own swizzles.

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
  - **`gotchas.md`** — the 265-entry transferable ledger. Every "gotcha N" resolves here.
  - **`port-history.md`** (what each session established) and **`open-items.md`** (the
    backlog, in order) — both split out of this file on 2026-08-08.
  - **`d3d-translation-plan.md`** — the renderer-architecture pivot, its recon tables and
    licensing, plus the per-phase build-out records. **The first read before any renderer
    work.** `d3d-kickoff.md` and `d3d-phase-c{,2..26}-kickoff.md` are the per-part
    hand-offs, each superseding the last; the newest is the live one. A kickoff's most
    valuable section is its list of the parts of that phase that **already exist** and
    would otherwise be rewritten from the plan text — write that section for every phase.
  - `phase1-notes.md` / `phase3-notes.md` / **`phase5-notes.md`** — the per-phase records
    of what the runtime work found that neither the plan nor the kickoff predicted, with
    `phase{1,3,5}-kickoff.md` the matching hand-offs and `runtime-plan.md` the phase plan.
  - **`phase-av-notes.md`** (sound, the cinematic that was waiting for it, and part
    29's diagnosis of the loop that followed) with `phase-av-plan.md` the plan it
    executed, `phase-av-kickoff.md` the phase hand-off and `part29-kickoff.md` its
    successor. **`part32-kickoff.md` is the LIVE one** and supersedes all of them on
    "where the port is". `d3d-phase-c28-kickoff.md` records how the white-surface chain
    was built, but **two of its eight steps are retired and its item 0 is answered** —
    read `phase5-notes.md` §6ba before following anything in it.
  - `instruments.md` (every env var and arm), `measurement.md` (how to judge a change),
    `perf-cpu-plan.md` (the live performance plan) and `perf-plan-overnight.md` (its
    executed predecessor).
  - Formats and tooling: `big-archive-format.md` (the cracked `.big` container, **plus
    two part-27 retractions: the name table is NOT fixed-width outside the shader
    banks, and 1,671 of 12,481 entries ARE compressed** — read it with
    `tools/big_list.py` and `tools/big_decompress.cpp`),
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
    calling the title's callback at 5.333 ms/frame, measured at 187.4-187.6/s against
    the 187.5 that 48 kHz needs), the XMA context array + its MMIO register file, and
    **the XMA DECODE WALK**. Finding 36; every structural claim is quoted from the
    guest function that states it, and the context layout is checked against the
    guest's own arithmetic rather than against Xenia's struct (`dw[8] - dw[7] = 6400
    = 25 blocks x 256`). **Read the physical-address note before touching a context
    pointer** — that one thing was the whole audio defect.
  - `audio/{xma_decoder,audio_out}.{h,cpp}` — phase A/V. ffmpeg's `AV_CODEC_ID_XMA2`
    (lifted from the Fable 2 port, so the licence is ours) and the SDL device.
    Deliberately dumb: the title's own mixer sums every voice into the one 5.1 frame
    it submits, so anything wrong with the SOUND is upstream of `audio_out.cpp` and
    `CZ_AUDIO_TRACE`'s peak is what tells the two apart.
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
**AND THAT CHECK ONLY FIRES FOR A SHADER A RUN ACTUALLY BINDS.** Part 27 found
`ps_7d6044e7dcaea1f2` sitting in `~/DR2CZ-troubleshooting/ucode-dumps` and MISSING from
the built cache — microcode we have held for sessions and never translated. No run had
bound it, so the counter read 0 every time, and the module COUNT could not show it either
because the cache also carries `ps_926c15dd20571cf1`, whose microcode is lost: 410 dumps,
410 modules, different sets. **The gate is to rebuild from the dumps and compare the
NAMES, not the count** — it is free and it is two lines:
```
tools/build_shader_spv.sh ~/DR2CZ-troubleshooting/ucode-dumps /tmp/spv_check
diff <(ls /tmp/spv_check/*.spv | xargs -n1 basename) \
     <(ls assets/shader_spv/*.spv | xargs -n1 basename)   # only the lost-microcode entry may differ
```
And the gate on the sidecars themselves, which is **two-sided by construction** — the
per-slot texture dimension is derivable both from our ucode parse and from DXC's
`OpDecorate ... DescriptorSet` words, so a disagreement means one of the two decodes is
wrong. Run it after any cache rebuild; exit 1 is a real defect:
```
python3 tools/shader_dim_census.py             # 310 modules 2D, 94 cube, 0 disagreements
```
It also names the sidecars carrying no `tfetchDims` at all — cache entries built before
part 25 whose microcode is gone. **Keep ucode dumps in `~/DR2CZ-troubleshooting/ucode-dumps`,
not in `/tmp`**, which is a tmpfs: eleven entries were lost that way and two operator runs
(the military arrival, then Still Creek end to end) recovered TEN of the eleven. The last,
`ps_926c15dd20571cf1`, samples only sets 0 and 3 — an ordinary 2D shader, so nothing
depends on it. A lost dump is a location nobody has replayed, not a permanent loss.
**The cache is 430 and it has grown on EVERY session that reached new ground.** 335 from
the captures, 337 with our own dump, then 339, 353, 370, 371, 391, 394, 397, 402, 409, 411,
419, 424, 430 — 23 of those from two operator play sessions on 2026-08-08 alone, and
**11 more from part 39's operator evening, of which THREE were never reported missing by
any run and only the name-diff gate found them**, once the whole-frame black
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

Look inside the game's `.big` archives — 146 of them, 12,481 entries:
```
python3 tools/big_list.py --all --find cc_          # search every archive by entry name
python3 tools/big_list.py <a.big> --extract <name> --out DIR
./tools/build_big_decompress.sh                     # once; links XenonRecomp's own LZX
tools/big_decompress <extracted> <out.bct>          # 1,671 entries are compressed
```
**`--extract` writes the STORED bytes**, which for a compressed entry is the compressed
stream and not the asset — it says so when that happens. `big_decompress` checks its own
output against an oracle (every loose `.bct` on disc opens `05 01 01 E2`) rather than
asking anyone to eyeball it. Both tools NAME what they could not parse instead of skipping
it: the first version of `big_list` silently dropped 95 of 146 archives, which would have
answered "is this asset here" with a confident no.

Build the runtime (needs `clang++`, **SDL2, Vulkan and ffmpeg**; ~90 s on 16 cores for a
cold image build). All three are required rather than optional-with-a-fallback, and for
one reason: **each of them, if it silently went missing, would present as a defect in the
game rather than as a build problem.** A lost window looks like input that stopped
working; a lost renderer looks like a black screen; a lost `libavcodec` looks like the
mute game this port shipped for 28 parts. Requiring them at CONFIGURE time is how those
stay build messages. It is safe to require all three because each is off at RUN time
unless asked for — `CZ_VKDRAW=1` for the renderer, `CZ_NO_AUDIO_OUT=1`/`CZ_NO_XMA_DECODE=1`
to switch audio back off — and `-DCZ_WINDOW=OFF` is how you say "headless on purpose" out
loud. On Fedora: `sudo dnf install SDL2-devel vulkan-loader-devel vulkan-headers
ffmpeg-devel`.
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

**REACH THE OUTDOOR WORLD AND A CROWD VIA THE TITLE'S OWN DEBUGJUMP SCREEN. This is the
recipe to use** — it is the operator's route, it is anchored to an EVENT rather than to a
wall clock, and it lands Chuck by the military camp in a full crowd at **7,431 draws** in
under half a minute of menu work:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE \
  CZ_VK_FRAME_STATS=/tmp/out.txt timeout 420 ./cz_runtime > /tmp/out.log 2>&1)
awk 'NR>1 && $2>m {m=$2} END {print m}' /tmp/out.txt      # >= 6,000 = it got there
grep -E "WAITJUMP|requested DebugJump" /tmp/out.log       # the four lines that prove it
```
`F2` opens the shipped DebugJump screen (needs `CZ_DEBUG_MENU=1`); `DOWN` once selects
`Case 0-2`, which spawns outside. **`WAITJUMP` is the load-bearing part**: the DebugJump
request is HELD until the frontend exists and lands whenever it lands (27 s here, 131 s on
another boot), so the barrier parks the sequence — repeating the preceding entry, which is
why `START` precedes it — until the screen is actually up, then starts the remaining
intervals from that moment. That is what makes this reproducible where a fixed-time recipe
is a fit to one afternoon (gotchas 75, 251).

**FULLY UNATTENDED, with the AI driving and the map handled** — this is the form to use
for texture coverage or any long roam:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER \
  CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE \
  CZ_SHADER_DUMP=~/DR2CZ-troubleshooting/ucode-dumps timeout 600 ./cz_runtime > /tmp/o.log 2>&1)
grep -E "WAITJUMP|EXPLORER engaged|changed the state away|pressing B" /tmp/o.log
```
Three things it handles that took a session to find, each measured rather than reasoned:
**the title's AI rewrites the state you set** (we wrote ITEM PICKER twice; a live read of
the running process found MISSION MASTER), so `CZ_AUTOCHUCK` re-asserts and counts the
overrides — 3 in an 8,657-frame run. **The AI opens the MAP by itself** about two minutes
into a roam and parks the run on it — not our input (a BACK-delivered counter reads 0, and
`CZ_FAKE_START_MS` bypasses the real pad entirely), so it is closed by hash
(`06903E1A`/`890DF3E5`, found with `CZ_SCREEN_TRACE=1`). **The close is B, not BACK** —
BACK is what opens the map. See `docs/instruments.md` for all four variables.

**Why this replaces the stick recipe below for anything comparative:** two arms of a
picture A/B are only comparable where their `drawFingerprint` and `cameraFingerprint`
agree, and with the stick recipe that filter yields 13-44 frames of ~300, every one under
1,800 draws — i.e. the outdoor era was never comparable at all (gotcha 247). Add
`AutoChuck` from the debug menu for coverage, or leave the character standing still, which
should hold the camera fingerprint matched across arms for long stretches.

The older stick recipe, kept because it walks a DIFFERENT route (out of the safehouse into
Still Creek, with a camera sweep) and because several recorded measurements were taken on
it — Chuck walks out of the safehouse into
Still Creek and the camera sweeps. It parks in the safehouse at
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
CZ_VK_FRAME_STATS=file   one line per presented frame; input to tools/frame_compare.py,
                   tools/frame_determinism.py (is a matched comparison possible at
                   all?) and tools/frame_era_medians.py (the outdoor A/B)
CZ_VK_FRAME_DUMP=dir     every 64th frame as a PPM — the picture, self-servable
CZ_VK_SNAP_DUMP=dir      EVERY resolve snapshot of one frame: which PASS went wrong
CZ_RING_TRACE=1    the ring, the brake's health, and the GPU/CPU hand-off chain counted
                   link by link. `truncated=0` is a standing gate
CZ_FILE_TRACE=1    every open/read, including the not-founds
CZ_WAIT_TRACE=1    name any infinite wait that outlasts 5 s, with guest callers
CZ_FAKE_PRESS_SEQ=...    synthetic input. MANUFACTURES PROGRESS — never a gate run
CZ_GUEST_LOG=1     the engine's OWN debug printf (640 callers). The sink — pair it
                   with CZ_GUEST_DIAG or it prints only the ungated errors
CZ_GUEST_DIAG=1    **the switch.** One byte, `0x829EC974`, read by 2,013 sites and
                   written by none, shipped as 1: a release KILL SWITCH, not the
                   "flag per category a shipped build left at zero" that gotcha 215
                   guessed and gotcha 266 corrects. Clearing it (plus `0x82AC3EAD` so
                   the un-silenced asserts print rather than trap) takes the outdoor
                   route from **0 `[guest]` lines of 11,168 to 1,239** — null and
                   positive control in one pair of runs. It is what makes the streaming
                   and zone-LOD decisions readable. A DIAGNOSTIC ARM ONLY: 2,013
                   formatting sites on the frame path, so never quote a frame time
                   from a run with it set (gotcha 7)
CZ_NO_XMA_DECODE=1 **the control arm for sound.** Contexts allocate, the pump runs,
                   nothing decodes — the runtime as it was for 28 parts. It is also the
                   arm that showed the prologue cinematic was waiting on audio
CZ_NO_AUDIO_OUT=1  the whole pipeline, no device. Separates "the guest produced audio"
                   from "we played it", and it is what a headless gate run wants
CZ_XMA_DECODE_LOG=1  per-context decode activity, and REFUSED split from short
CZ_SHADER_DUMP=dir put this on any run that might reach new ground, including an
                   operator run: a missing shader is one log line and a silent counter.
                   **NEVER point it under /tmp** — that is a tmpfs and it is why eleven
                   cache entries have no microcode left. Use
                   `~/DR2CZ-troubleshooting/ucode-dumps`
CZ_VK_NO_CUBE_SNAPSHOT=1  decline the cube map the TITLE RENDERS ITSELF to the white
                   dummy — the part-25 renderer in the part-26 binary, and the control arm
                   for the cube snapshot path (35.9% of cube fetches on the outdoor route)
CZ_VK_NO_CUBE=1    bind cube fetches the pre-part-25 way (into the 2D array, so the shader
                   samples the white dummy). The same-binary control arm for the cube-map
                   work, and the one to hand an operator for a side-by-side
CZ_VK_CUBE_POISON=1  the cube dummy becomes MAGENTA — the positive control that showed the
                   cube sample reaches the presented image (80 of 110 frames, up to 72% of
                   a frame's pixels). Read it with a per-pixel diff against the unpoisoned
                   run, NOT by looking for magenta: the sample TINTS, and the semantic
                   detector read 0.24% where the diff read 80 of 110 (gotcha 248)
CZ_VK_DIM_CENSUS=1 where the DIMENSION lives in a texture fetch constant, answered by
                   partitioning fetches on the shader's independent answer (gotcha 244)
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
- **No Bink** (finding 7). Grep `.big`, never `.bik`. **And "movie" is the wrong word:
  part 27 read the archives and a cinematic is an IN-ENGINE SCRIPTED SCENE** — 29 `.txt`
  scripts in `data/cinematics/cinematics.big` naming a camera animation, actor animations,
  particles, a HUD event and one audio event, with the animation data in
  `data/anim/cinematic/*.big`. There is no codec to write. `docs/phase-av-plan.md`.

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

Where the port is, as of 2026-08-15 (part 44 — ITEM 00i IS CLOSED AS FAITHFUL,
and the part-43 menu reframe is retracted):

* **ITEM 00i CLOSED: fresh hardware shows the flat-at-range class at the same
  rate we do; the "hardware is full everywhere" oracle was a warm loaded-save
  session.** Two censuses (`phase5-notes.md` §6bx + addendum): our MENU frame
  equals B1's title era bind for bind (the 8×8-on-big-meshes draws are
  `flat_color_gray_cm.bct`, 346 bytes, flat BY DESIGN — part 43's "the defect
  is the set APPLY" premise failed its never-run control); and B2 — a FRESH
  hardware session walking into Still Creek, on disk since day one — runs
  2–3% tiny-on-big through the whole town era with the world shader binding
  an 8×8 on thousands of draws, persisting to session end (no promotion wave
  exists fresh). The texture LEVEL MACHINE is fully named and verified healthy
  on our runtime (name hash, DB entry layout, promote/bind/catch-up walks,
  levels: 0 = full set payload, 1 = thumbnail); `CZ_SET_APPLY_PROBE=1` prints
  every gate of it. Operator comparisons must be like-for-like (fresh vs
  fresh, or same save on both). The one open curiosity: WHAT a save carries
  that makes a warm session all-full (skip bits the candidate) — nameable by
  loading the operator's tanker save with the probe, not a defect either way.
  **Big-trace censuses OOM the stock `xtr_draw_bindings.py` three ways** —
  the validated lean variant (rolling memory window + streamed CSV,
  byte-identical on B1) lives in `~/DR2CZ-troubleshooting/part44/`, and a
  multi-GB CSV must never target /tmp (the tmpfs filled mid-run and every
  shell command "died" until space was freed). **`docs/part45-kickoff.md` is
  the LIVE hand-off**; the sledgehammer-pickup FREEZE (signal-15, next
  operator session carries `CZ_WAIT_TRACE=1`) is the top item.

Where the port was, as of 2026-08-14 (part 43 — the zone texture-set decision is
fully named, OUR EXECUTION OF IT IS CORRECT, and item 00i waits on capture R5):

* **ITEM 00i INVERTED: the engine ITSELF picks LOD at the spawn, on inputs
  verified live.** The whole chain is named (`phase5-notes.md` §6bw):
  `sub_82270870` picks `COMMON_TEXTURE_LOD.tex` iff the zone's LOD-capable
  flag (rec+0x90C, from the two files' sizes at setup) AND every volume in
  the zone's list is farther than its threshold from the camera `[g+0x40]`
  (per-level boost tables; level 14 = none; force byte 0x82A57BD7 = 0).
  `CZ_ZONE_TEX_PROBE=1` printed every input at decision time and predicted
  part 42's narration line for line; `tools/zone_lod_live.py` reproduces the
  verdict on a live process. Zones 1/2/3/7 are all-far by 31-107 m at the
  spawn, the camera is ALREADY at the spawn when the burst runs, and the
  ordering hypothesis is dead (menu/origin cameras yield MORE LOD zones).
  **The LOD file IS the thumbnail set by design** (27 KB vs 1.3 MB). The
  decision runs ONCE per zone load; no promotion trigger found statically,
  and no headless roam has crossed a LOD threshold to test it live. **The R4
  traces cannot adjudicate this — they are a WARM-session sweep at Big Buck.**
  Capture **R5** is filed (one fresh DebugJump stand-still F4 at the spawn,
  no patching): flat street → 00i collapses to a state-comparison artifact;
  full street → suspects in order: volume skip-bits, volume data, an unfound
  reload path. **Build no fix before R5** (gotcha 5 — every candidate fakes
  the decision). `docs/part44-kickoff.md` is the LIVE hand-off.
* Instrument-log gotcha paid for: the probe's first version printed a
  directory object as %s, salted the log with NULs, and plain grep read the
  whole probe as absent for two runs — `grep -a` / `tr -d '\0'` before
  believing a zero from an instrument's own log (gotcha 25's self-made form).

Where the port was, as of 2026-08-14 (part 42 — the flat-texture class is a
PROMOTION-DENIAL defect, and the DoF "hardware contradiction" mostly dissolved):

* **ITEM 00i IS CORNERED: a streaming PROMOTION-DENIAL decision, not a rate.**
  Verified pre-post-chain (the 003053 building is patternless in the SCENE
  surface); the flat class is a handful of shared world atlases stuck at 8×8/16×16
  (53 big draws from TWO addresses in one frame); hardware binds tiny-on-big
  **0 times across all eight R4 traces** where we do it in 44/82 walk frames; and
  **standing still for 2.3 minutes promotes NOTHING** — which refutes the rate
  class outright (file-IO latency, and the `KeSetBasePriorityThread` no-op that
  had been the named candidate since part 28 — do not build it for this item).
  **The engine then named it itself** (§6bv addendum): the flat class IS the
  per-zone `COMMON_TEXTURE.tex` vs `COMMON_TEXTURE_LOD.tex` choice — our runtime
  loads zones 1/2/3/7 as LOD at the spawn where hardware's street is fully
  textured. `ForceLODTexForStreamingWorld` refuted by a live read (0x82A57BD7=0).
  Next: the branch that picks the filename, then fix ITS INPUT (never the
  branch). **`docs/part43-kickoff.md` is the LIVE hand-off.**
* **ITEM 0u IS DOWNGRADED — the composite behaves as designed on BOTH platforms.**
  Our constants match hardware's to the digit (`CZ_VK_PSBIND_PC` — the
  instrument already existed); the worked alpha math gives ~95% blur at 50 m on
  both sides; the gather radius is ~0 on both sides (blur640 ≈ the half-res
  downsample); and **hardware's own R4 PNGs are soft at range** — part 41's
  "legible storefront" was signage contrast read against OUR flat walls, i.e.
  00i wearing 0u's clothes. Remaining residues: the fmt6 byte-split depth
  serving (edge weights only) and the gather's pc255.x (ours 0, hardware
  unrecoverable — loads from CPU-written `032B6000`, NOT a resolve destination;
  `xtr_draw_constants.py` now prints load provenance).
* **The cache gate paid again: 430 → 435.** Four dumped-but-never-built shaders
  (only the name-diff gate can see them) plus `vs_c8e86dffb37149dd`, which
  exposed and fixed a XenosRecomp emitter bug — a vfetch with an EMPTY
  destination mask emitted `r0. = ...;` and had been the bank's only
  translation failure. `docs/xenonrecomp-upstream-bugs.md`.

Where the port was, as of 2026-08-14 (part 41, second session — the far-field
complaint is LOCATED: it is the DoF COMPOSITE, and the scene under it is sharp):

* **CORRECTED BY THE OPERATOR: their complaint is the FLAT-TEXTURE class (00i),
  not softness** — buildings/objects render with NO surface pattern at range
  (their captures 003053/003368). 00i is back on top; the census tracker failed
  to pair faces across the walk, so the next move is CZ_VK_DRAW_ID at a
  reproduced flat-building view, then the binding comparison against R4 at
  matched distance. What the session ALSO found (secondary, own evidence):
  **the far-field SOFTNESS is the post chain, not sampling** — their 20 F9 captures bisect it: the resolved scene
  surface is crisp at every distance, the DoF composite (lerp by the blur
  surface's alpha) is what the player sees. Our real scene depth (0.83..1.0)
  saturates the game's own CoC formula by ~50 m; hardware runs the SAME shader,
  constants and depth format yet stays legible at 40-60 m — **that compensating
  term is UNLOCATED and blocks any fix** (do not clamp CoC; name the term).
  `docs/part42-kickoff.md` is the LIVE hand-off; §6bu the full anatomy, including
  the retraction of an hour of arithmetic done on a memory record that turned out
  to be the previous frame's PICTURE (gotcha 311 — 280's second disguise).
* One certain sub-defect either way: the DoF gather reads depth AS 8_8_8_8 bytes
  and we serve the float image (part 36's counted-unclaimed 231-fetch class).

Where the port was, as of 2026-08-14 (part 41, first session — the far field: per-fetch
samplers and the packed mip tail, both default):

* **PER-FETCH SAMPLERS ARE THE DEFAULT (d5b8fdc) — the fetch constant's own
  mag/min/mip/aniso fields, honoured for the first time since phase 5.** The world's
  albedo filters at the 4:1/8:1 the title asks for; the shadow atlas gets the
  point/point/point/no-aniso sampler hardware asks for. Confirmed by a three-arm
  same-binary A/B: outdoor sharpness +2.6% with NO overlap at three runs an arm, no
  era regression. `CZ_VK_NO_FETCH_SAMPLERS=1` is the part-40 arm; `CZ_VK_ANISO=N`
  caps the degree. **The GLOBAL-aniso form was tried first and is REFUTED BY
  PICTURE — do not re-buy it**: 16x on sampler 0 speckles the whole frame dark,
  because sampler 0 also serves the shadow-map lookups and hardware fetches that
  atlas with aniso=0/POINT (621-fetch census, §6bt). The sharpness metric read the
  regression as a -19% DROP — a metric can flag a defect while mislabeling it; the
  matched F9 eyeball named the mechanism in one look.
* **THE PACKED MIP TAIL IS DECODED (409777d) — part 39's standing decline, paid.**
  No remembered table: `tools/packed_mip_derive.py` brute-forced the offsets from
  hardware's own bytes over all eight R4 traces — a square packed level of width W
  blocks sits at block (W,0) in the shared tile, 7,466 of 7,515 informative votes,
  both DXT formats. ~4,468 tail levels upload per outdoor run (73 guard-rejected,
  302 mostly-empty — everything underived still declines and counts).
  `CZ_VK_NO_MIP_TAIL=1` is the tail-only part-39/40 arm. Era statistics unresolved
  (like the chain itself in part 39); justified on correctness.
* **`docs/part41-kickoff.md` remains the LIVE plan** — items still open: the
  operator's far-field verdict (their mandate started this part), 00i pairing on
  the 81-capture walk (item 3), A2M dither at distance (item 4), clamp modes /
  edge fringes (item 5 + 1b's clamp half). `docs/phase5-notes.md` §6bt is the full
  record of this session.

Where the port was, as of 2026-08-13 (part 40 — the shard trees SOLVED, and the cause
was one wrong register index):

* **RB_COLORCONTROL IS 0x2202, NOT 0x2205 — and the whole shard-tree class (item 0t)
  falls out of that one constant.** `xenos.h` guessed the per-RT blend controls
  contiguous at 0x2201..0x2204; the real map interleaves them
  (0x2201/0x2205/0x2209/0x220D) with COLORCONTROL at 0x2202, settled by histogramming
  both indices over an R4 trace (0x2202's values carry the 0xAA alpha-to-mask offset
  signature; 0x2205 never sets the enable bit). Consequences unwound: part 38's alpha
  test NEVER FIRED (its counter read zero in every log, unread), and part 39's
  "hardware never enables the alpha test in 40,703 draws" measured the WRONG REGISTER
  — the truth is **4,975 of 40,703 (12.2%)**: foliage, fences, hair, horizon sheets,
  and 1,787 draws of the shadow-caster shader whose whole body is "sample alpha, clip
  against RB_ALPHA_REF". With the index fixed the trees have cutout leaves and lost
  their dark plates (the caster now clips leaf holes into the shadow map instead of
  stamping the solid quads whose projected shadows WERE the plates). Same-binary arm:
  `CZ_VK_NO_ALPHA_TEST=1`. §6bs, gotcha 308, `open-items.md` 0t.
* **The path there is reusable:** a headless viewpoint FACING the trees (DebugJump
  spawn + two RSLEFT camera holds + synthetic F9 — F9 works in `CZ_FAKE_PRESS_SEQ`),
  then a three-arm A/B in which `CZ_VK_NO_DEPTH_FETCH=1` lit the plates (darkness =
  the shadow term) while still showing opaque cards (cutout separately missing), and
  the F9 atlas snapshot showing the solid diamond cards the caster stamped.
* **`CZ_VK_TEX_DUMP` can finally see DXT textures** (it was gated to 8-bit formats
  for its whole life — blind to nearly every texture in this game), and
  `CZ_VK_TEX_DUMP_PS=<hash>` dumps a material's textures keyed by SHADER, which
  survives a reboot where a guest address does not (part 39's foliage addresses,
  replayed by address, decoded as BARBED WIRE — gotcha 306).
* **Parts 40's second half added two more cutout fixes**: strict GREATER (ref + 1/512
  — the foliage cuts out OPAQUE cards at ref 0.0 and the emitted clip is >=-shaped)
  and EQUAL@1.0 emulated exactly (ref - 1/512; the shadow-caster cutout, 174 draws,
  which was blacking canopy interiors). A2M-without-test stays counted; EQUAL below
  ref 1.0 stays counted.
* **THE FAR FIELD IS THE OPEN FRONT, and `docs/part41-kickoff.md` is THE DISTANCE
  PLAN and the LIVE hand-off**: anisotropic filtering (never enabled), the packed mip
  tail (still declined), item 00i's streaming pop (now pairable against the 81-capture
  ~1 m-spaced operator walk in `~/DR2CZ-troubleshooting/part40-operator/verify/`),
  A2M feathering at distance, then edge fringes/exposure/fog.

Where the port was, as of 2026-08-12 (part 39 — the mip chain, an input the renderer
declared and then discarded for the whole of phase 5):

* **THE GUEST'S MIP CHAIN IS UPLOADED NOW, AND IT WAS NEVER READ BEFORE.** A Xenos
  fetch constant names TWO addresses: dword1's base holds level 0 alone, dword5's
  `mip_address` holds levels 1..n. `DecodeTextureFetch` parsed `mipMin`/`mipMax` from
  phase 5 onward, **nothing read them**, and `CreateImage` hardcoded `mipLevels = 1` —
  so every minified surface in the game sampled full-resolution texels at whatever rate
  the rasteriser landed on. Hardware carries a mip-chain address on **88,689 of 328,164 fetches (27.0%) across
  all eight R4 traces**, declaring chains up to **nine levels** deep; our own outdoor
  frame declares one on **69.6%** of fetches. The layout was verified level by level against hardware's own bytes (same
  mean, steadily fewer distinct colours) rather than reasoned about; the **packed tail**
  is declined and counted, never guessed. `CZ_VK_NO_MIPS=1` is the same-binary control
  arm; 1,815 textures take a chain on the outdoor route. A divergence guard shipped with
  it caught the rule's limit at once — **254 of 1,818 chains held a level that is not that
  texture**, so it REJECTS rather than counts (gotcha 301). **The A/B was run TWICE and the first
  result was the bug**: with the 254 bad levels bound it read mean luma −1.35% (resolved,
  and moving toward hardware's darker frames); with them rejected it reads **+0.36%,
  unresolved**. So the chain produces no resolvable era-statistic change and is justified
  on correctness alone, and **item 00i is untouched by it** — the packed tail, where the
  deepest minification lives, is still declined. Gotcha 295 is the transferable half: **for every field a decoder
  parses, grep for a READER** — and 298: **"distinct colours" rewards ALIASING**, so the
  registered prediction had its sign wrong. §6bq.
* **Item 00i's level-0 input is EXONERATED** — the Big Buck sign draw's texture bytes
  are **md5-identical** to hardware's from a different boot at a different address — and
  `mip_min_level` is **0 on all 328,164 hardware fetches**, refuting "streaming raises an
  LOD clamp we ignore". Whether the mip chain closes 00i is a registered prediction
  against an era-median A/B, not yet a claim.
* **Item 0t's suspect is REFUTED**: across all eight R4 traces (**40,703 draws**)
  hardware enables neither the alpha test nor **ALPHA-TO-MASK**. Do not build the
  emulation. The item now needs one round-5 trace **standing at a shard tree**.
* SIGTERM/SIGINT dump the renderer counters — gotcha 294's headless twin (gotcha 297).
* **`docs/part40-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-12 (part 38 — the operator evening: the random-texture
class fixed, two defects cornered with hardware ground truth):

* **THE TEXTURE CACHE REVALIDATES BY DEFAULT — the "random texture on everything up
  close" class is fixed and operator-confirmed.** The once-only upload cache served a
  streaming-recycled address's first occupant forever (a tanker wearing a BRICK WALL;
  guest memory holding a pickup atlas by dump time). Part 35's "4 stale of 92M" was a
  short-route census (gotcha 293). `CZ_VK_NO_TEX_REVALIDATE=1` is the control arm.
  This also retro-explains item 0s's per-boot "wrong quality level" lottery. §6bp.
* **The part-37 class-closure tour confirmed**: Dick at distance and the pawnshop
  boards clean on the fixed renderer.
* **The RB alpha test is built and driven** (pipeline-key bit + spec constant +
  RB_ALPHA_REF per draw; unknown funcs counted, never guessed) — and the SHARD TREES
  are NOT it: foliage never fires the RB alpha test. Suspect ALPHA-TO-MASK (bit 4);
  hardware's register state at the foliage draws is in `Xenia logs/R4_world/`.
  New item 0t. `CZ_VK_NO_ALPHA_TEST=1` is the arm for what was built.
* **Item 00i IS OURS AND IS NOW THE TOP PICTURE ITEM**: the operator walked the Big
  Buck approach on our renderer (flat-color building panels at range) and delivered
  `R4_world/` the same night — eight frame-locked hardware traces of the same walk,
  fully textured buildings at every distance. Eight paired oracles for the fix.
* Window-close now dumps the renderer counters (gotcha 294 — a whole session's
  census was lost to the one exit path a human actually uses).
* **`docs/part39-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-12 (part 37 — the striped-material class is solved):

* **ITEM 0s'S BLOTCH MECHANISM IS FOUND, FIXED, AND ON BY DEFAULT.** The
  black/white-banded "striped material" garbage (tanker close-up, Dick at distance,
  pawnshop boards) was never in any texture: the runtime's own 16-bit texcoord
  unswizzle mask double-corrected fetches whose microcode already carries the
  compensating `.yx` destination swizzle, so baked-LIGHTMAP UVs arrived transposed and
  the lightmap's black prop-shadow shapes painted the surfaces. Mask now defaults to
  zero (= hardware semantics; the fix), `CZ_VK_TEXCOORD_SWAP=1` repaints the blotch as
  the same-binary control arm. Named by content-ID of the tanker draw (all of whose
  inputs are md5-identical to hardware's), semantics read from Xenia's own disassembly
  of the same microcode, verified by matched-index F9 A/B at the reproduced site — the
  blotch site IS the Case 0-2 DebugJump spawn, so the whole loop ran headlessly.
  `docs/phase5-notes.md` §6bo; gotchas 291 (identify draws by CONTENT, not vertex
  count) and 292 (model the full state chain before adding a runtime correction).
  **`docs/part38-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-12 (part 36, second half — the reproducibility layer):

* **AN F9 CAPTURE NOW RECORDS WHERE YOU WERE STANDING, AND TEXTURES CAN BE ISOLATED
  WHILE AN OPERATOR PLAYS.** Every picture finding in this port had been anchored to
  "the operator walked somewhere and pressed F9", which nothing can reproduce. The
  capture now writes `capture_<frame>.pose` beside the picture: the player's world
  position, read through the shipped debug console's own path (**the position is
  `obj + 0x1C`**, reached by a five-step lookup `setplayerpos` and `getplayerinfo`
  share), plus the camera constants. Read it with `tools/pose_read.py`.
  `CZ_VK_TEX_FILTER_FILE` isolates or hides a texture **live, no relaunch**, which the
  latched env vars could never do for a defect that picks a different quality level per
  boot — and **streaming addresses are stable across boots** (703 of 712 shared
  addresses held identical content between two sessions), so a census from one boot
  names textures usable in the next. **A teleport is diagnosed but unfinished**: the
  crash was the wrong THREAD (the engine's per-thread context lives in TLS slot 8 and no
  input-polling thread has it — gotcha 289), fixed by hooking the accessor itself; but
  the actor's position fields are OUTPUTS the engine rewrites every frame (gotcha 290),
  so the working placement path is DebugJump's own spawn code, not `setplayerpos`.
  `docs/phase5-notes.md` §6bk-§6bn; **`docs/part37-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-12 (part 36, first half):

* **ITEM 0s IS REFRAMED: THE "JUNK SHEETS" ARE CORRECT TO THE BYTE, AND THE WRITER
  HUNT IS CLOSED BEFORE IT STARTED.** Part 36 ran the R3-oracle comparison first, as
  ordered: our live-dumped 400x240 and 1024x64 impostor sheets at blotch time are
  **md5-identical to the bytes hardware's GPU sampled** for the same material in
  `tanker.xtr`, and decoded properly (`tools/tex_decode.py`, new) they are coherent
  billboard alpha-cutouts — white colour endpoints, content in ALPHA, exactly what
  part 35's junk-scorer was guaranteed to misflag (gotcha 287; the kickoff's own
  warning said to decode and LOOK first). The kickoff's "hardware binds only 3 DXT5,
  0 DXN" was a filtered pass — the full census is 3,514/3,040, hardware draws the
  whole sheet class. Item 0s is now a WRONG-BINDING question (a real asset at the
  wrong streamed quality level — the "weird" 110AD000 texture is structured real
  content absent from hardware's frame), with the blotched-draw identification as
  step one and hardware's 16 small colour resolves (never in our resolve set;
  resolve write-back still unimplemented) as the standing lead for the remaining
  sub-defects. `docs/phase5-notes.md` §6bj; **`docs/part36-kickoff.md` is the LIVE
  hand-off.** Content-match census: 226 of 459 blotch-frame textures byte-identical
  to hardware's frame; the unmatched are unadjudicated, not suspects (gotcha 288).

Where the port was, as of 2026-08-12 (part 35):

* **THE WRONG-TEXTURE REPORTS COLLAPSED INTO ONE ITEM, AND IT IS FULLY
  EVIDENCE-BOUNDED: open-items 0s, THE STRIPED-MATERIAL CLASS.** One streamed quality
  level of an asset renders as black/white banded garbage (the tanker close up, Dick
  the survivor at distance, the pawnshop's window boards). An operator session plus
  live-process texture dumps taken seconds after each F9 (`tools/live_texdump.py`,
  gotcha 285) established: guest memory GENUINELY holds the garbage at blotch time,
  and **every reader of the bytes is measured innocent** — five theories died in one
  session (the shadow term; a VFS positional-IO race, fixed on principle in d65874d
  with its prediction honestly retracted, overlap counter 0; a stale texture cache,
  content guard 4 of 92,730,622 hits; the snapshot age fallback; one retracted
  misattribution). The affected textures include CPU-composed impostor sheets that
  exist nowhere on disc and are never resolve destinations. **The next move is named
  (gotcha 286): trace the WRITER** — and the oracle arrived the same night:
  **`Xenia logs/R3_world/`**, four frame-locked single-frame traces at exactly the
  four defect sites, hardware clean at every one, each trace carrying the bytes the
  sheets SHOULD hold. `docs/phase5-notes.md` §6bi; `docs/part35-kickoff.md` is the
  LIVE hand-off.
* **Item 3d (NPC part meshes) is CLOSED** — Dick renders whole on two binaries; the
  missing parts were the shader-cache gap, exactly as the item's re-test note
  predicted. **Item 00i (LOD placeholder pop) is captured** (flat colour panels ->
  full siding, reload_test 30631/30807) and one deliberate Xenia look from a verdict.
* **NtReadFile/NtWriteFile are atomic per handle now** (d65874d) — NT's contract,
  previously seek-then-read on a shared FILE*. `CZ_FILE_RACY=1` is the control arm.
  Credited with nothing visible; kept on correctness.

Where the port was, as of 2026-08-11 (part 34):

* **THE 4x MSAA Y FACTOR IS THE DEFAULT — part 32's item 0, shipped, and the shadow
  cascade's two known defects are both fixed and on by default.** A Xenos 4x surface is
  a 2x2 sample grid — twice as tall in samples as well as twice as wide — and for 25
  parts only X had the factor at the window-coordinate site. The reconciliation that
  unblocked it (§6bh): a clear rect is in the CLEAR declaration's own pixel space, so
  both axes scale by the draw's OWN declared sample factors — no rule needs the render
  declaration at clear time, and the Y over-clear past a shorter surface is the same
  approximation the X factor has always applied to the 640x360 post surface.
  `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` is the same-binary control arm (the part-33
  renderer); the part-32 arm variable is retired. Gates: atlas **46.8750% -> 0.0038%
  zero, 512 -> 1024 covered rows** (§6bf's numbers to four decimals); no other surface
  regressed; outdoor era medians **distinct colours +8.30% at 5.2x the null**
  (registered prediction, commit e10df05); validation tally unchanged; capture-E
  **+0.958 identity**; A5 **exit 0, 3 permutation, 0 real**; `truncated=0`; both PM4
  oracles exit 0. **The operator's three-way verdict landed the same day and CLOSES
  open item 3**: one Case 0-2 crowd spot, F9 per arm — the default is *"perfect"* in
  their words (atlas 0.0006% zero at the capture); both control arms show hard-edged
  black false-occlusion blotches on the same truck (46.875% / 75% atlas zero). §6bh's
  verdict table; `~/DR2CZ-troubleshooting/part34-operator/`.
* **§6ba's EXPOSURE QUESTION IS DOWNGRADED TO CLOSED-PENDING-A-MATCHED-LOCATION.**
  `CZ_VK_EXPOSURE_TRACE` on all three outdoor runs: every arm identical (the shadow
  change does not move the controller), frame 3000 reads 0.211 (part 31: 0.2146), era
  range **0.200-0.354**, mean 0.2755 — and hardware's **0.298 / 0.331** sit INSIDE that
  adaptive range. "Ours pinned where hardware reads 0.33" is not a property of the
  fixed renderer. The free completion: the trace beside an operator F9 at `w1_spawn`.

Where the port was, as of 2026-08-11 (part 33):

* **THE WHITE-SURFACE PLATEAU IS SOLVED — open item 00f, open since part 26, and the
  largest picture defect in the port.** The exact-`rgb(180,180,180)` surfaces were the
  shared tone epilogue evaluated at **`x = NaN`**: `max(NaN,K1)=K1` and `saturate(NaN)=0`
  make every NaN input land on `sqrt(K1*K2)` = 180/255, invariant under every constant —
  which is why parts 27-31's four whole-frame arms all read "unmoved" (gotcha 281: the
  laundering happens INSIDE the epilogue, so `isnan(oC0)` was blind by construction).
  The NaN entered at the VERTEX FETCH: this title wraps its fmt16 `k_10_11_11` packed
  normals as TEXCOORD (float4 input) while the runtime bound the attribute `R32_UINT` —
  a pipeline type mismatch (VUID 08733, 10 pipelines, 37 vertex shaders) that delivered
  the packed dword's bits AS floats: NaN wherever bits 30..23 are set, garbage normals
  on every fmt16 mesh otherwise, since phase 5. **One `CZ_VK_VALIDATION=1` run would
  have named it in part 26** (gotcha 282). Fixed in XenosRecomp (`XeUnpack_10_11_11`,
  read-site format branch) + runtime (fmt16 -> `R32_SFLOAT`): plateau **1,092 px -> 0**,
  scene mean luma 35.5 -> 44.7, distinct colours 80k -> 112k, validation 08733 10 -> 0,
  and the crowd's blotchy flat-lit patches are gone. The measurement chain (five paint
  arms, a 786,861-draw range census, and the no-test robust arm caught before its null
  was believed) is `docs/phase5-notes.md` §6bg. **The operator confirmed it the same
  day: all seven part-27 white-surface locations toured and captured on the fixed
  renderer, scene-buffer plateau ZERO in every one**
  (`~/DR2CZ-troubleshooting/part33-operator/`). Owed: a re-measure of the exposure
  discrepancy (ours 1.0 vs hardware 0.33) now that auto-exposure adapts to a correctly
  lit scene — §6ba's question may simply close. The NaN-input footprint was 17x the
  visible plateau, so LOD/00i, NPC part meshes and the shadow three-way should all be
  re-asked on this renderer.

Where the port was, as of 2026-08-11 (part 32):

* **HALF OF EVERY SHADOW CASCADE IS ZERO, AND A ZERO DEPTH SAMPLE READS AS OCCLUDED.**
  The atlas is **46.8750% zero in every band** — rows 512..1023 minus a 64-column sliver —
  on two routes and two frames, in four bands rendered from four different light frusta,
  so it is structural. The geometry is submitted for all of it (`CZ_VK_DEPTH_ALWAYS`:
  46.875% -> **1.86%**); the bottom half is REJECTED against the zero the EDRAM image was
  created with. **The cause is that a Xenos 4x MSAA surface is twice as TALL in samples as
  well as twice as wide, and only X has ever had the factor**: the title's two clear rects
  for the cascade — `(0,0)-(480,512)` on a 520-pitch 4x surface and `(960,0)-(1024,1024)` —
  tile the 1024x1024 map EXACTLY when both axes are scaled and cover **53.125%** of it when
  only X is, which is the observed coverage to four decimals.
  `CZ_VK_MSAA_WINDOW_SCALE_Y=1` -> **0.0038% zero**, title-screen picture unmoved. It is
  an ARM, not the default, because the SCENE tile's 4x clear wants X scaled and Y not, and
  that has to be reconciled first. `docs/phase5-notes.md` §6bf, `open-items.md` item 3.
* **THE HARDWARE ORACLE PART 31's SHADOW WORK WAS MEASURED AGAINST IS RETRACTED.** The
  16 MB `xtr_draw_bindings.py --dump-texture 1812F000` returns from `w1_spawn` is the
  PREVIOUS FRAME'S COMPOSITED SCENE — detile it and the game's HUD is legible, *"8
  KILLED"*. A `.xtr`'s memory records are snapshots with a time, so a capture **cannot**
  supply any surface the GPU produces inside the traced frame (gotcha 280, correcting
  gotcha 275's second half). The address-fold fix stands; the "hardware's is 96.5% full"
  yardstick never existed. The tool exits 2 on that dump now and prints *"a sound oracle"*
  otherwise; part 27's ground-texture comparison is unaffected.
* **`CZ_VK_NO_DEPTH_TEST` CANNOT ANSWER A DEPTH-PASS QUESTION AND ANSWERS THE OPPOSITE.**
  Vulkan ties depth WRITES to the depth TEST, so on a depth-only pass the arm empties the
  buffer — the very symptom it exists to rule out (gotcha 279). `CZ_VK_DEPTH_ALWAYS` is
  the replacement.

Where the port was, as of 2026-08-11 (part 31):

* **THE WHITE PLATEAU IS NOT THE TONE CURVE AT `x = 1`, AND FOUR PARTS OF WORK WERE
  PROSECUTED UNDER THAT READING.** Four whole-frame arms leave the pixels at exactly
  `rgb(180,180,180)` untouched: the sun `c24`, the additive `c67.w` term, the whole
  multiplicative path `c1.xyz` — which blacks out **61.5%** of the frame — and, decisively,
  `CZ_VK_PS_CONST_SCALE="14.w=0.25"`, which engaged on 11,835,619 draws, took the scene
  buffer from mean luma **35.07 to 18.30**, and put **zero** pixels on 119, where that
  curve sends `x = 1` at quartered exposure. A value produced by that curve cannot be
  invariant under scaling its exposure. **`180 = 255 * sqrt(0.5)` is now the coincidence
  to explain, not the explanation** (gotcha 277), and the next step is a PER-DRAW
  instrument — `CZ_VK_DRAW_CENSUS` plus `CZ_VK_SKIP_TEX` — because four arms in a row
  reporting "unmoved" is a fact about the instrument class (gotcha 276).
  `docs/phase5-notes.md` §6be, `open-items.md` 00f.
* **THE SHADOW ATLAS IS FIXED — `open-items.md` item 3, open since part 15.** The title
  packs four 1024x1024 cascades into one 4096x1024 atlas by pre-offsetting
  `RB_COPY_DEST_BASE` by 0x20000 each while leaving the scissor at the origin, and 0x20000
  is exactly +1024 texels in X in tiled address space. `DoResolve` understood the SCISSOR
  form of that idiom and not the ADDRESS form, so three quarters of every shadow lookup
  read zero. **Ours was 86.7% empty; hardware's copy of the same surface, dumped from the
  capture, is 3.5% empty.** Now 53.125% non-zero across all 4,096 columns against 13.281%
  across 1,024 with `CZ_VK_NO_ADDR_TILE_FOLD=1`, and `13.281% x 4 = 53.125%` exactly.
  **A surface you RENDER is still comparable** (gotcha 275) — the check took ten minutes
  and had been available since part 26.
* **THE GROUND SHADER'S 32 CONSTANTS ARE HARDWARE'S, and the constants are exonerated as
  a class.** Every register that is not a function of the camera matches to the printed
  digit, including `pc(21)` — a point light's WORLD POSITION — which is how the run proved
  it was in hardware's own lighting state without being asked to. All seven R2 captures
  answer; part 27's "`w1_spawn` cannot" was true only of `c253..c255`.
* Exposure is now readable per frame (`CZ_VK_EXPOSURE_TRACE`): ONE value is in force
  across a whole frame, to five digits. That had been assumed for four parts.

Where the port was, as of 2026-08-11 (part 30):

* **THE WHITE-SURFACE CHAIN HAS NUMBERS IN IT NOW, AND TWO OF ITS STEPS ARE GONE.** The
  tone curve every one of the 48 emitting shaders ends in is
  `out^2 = (max(0.25x + 0.75, 1) - saturate(1-x)^2) * 0.5` with `x = colour * pc(14).w`,
  and **our constants are hardware's to the digit** — a pre-registered prediction that
  they were not, refuted. So **180 is the value at exactly full exposure**, not a ceiling
  and not a gamma artifact: it is `sqrt(0.5)` written by the shader's own trailing `sqrt`.
  Our translation of `ps_ad65b98593f95926` is **instruction-for-instruction identical** to
  the capture's own disassembly, so the clamp is on an INPUT. Two retractions in place —
  the `k_8_8_8_8_GAMMA` reading of 180, and "these surfaces are not shaded at all" (the
  curve's derivative vanishes at `x=1`, so a 10% spread quantises to one 8-bit value).
  **What is owed is enumerated: the ground shader reads 32 pixel constants and nine have
  been compared.** `tools/xtr_draw_constants.py` reads hardware's;
  `CZ_VK_PS_CONST_SCALE="14.w=4"` is the arm that separates a pinned colour from one the
  curve is hiding. `docs/phase5-notes.md` §6ba, `open-items.md` 00f.
* **A CAPTURE THAT CANNOT ANSWER IS ONE CAPTURE, NOT THE SET.** Part 27 asked `w1_spawn`
  for the constants, got `UNRECOVERABLE`, and recorded that a new capture was needed; five
  of the other six answer, from data on disk for weeks (gotcha 274).
* **`timeout` WORKS AGAIN FOR HEADLESS RUNS WITH SOUND.** Since phase A/V it did not:
  `SDL_HINT_NO_SIGNAL_HANDLERS` sat below the `CZ_NO_WINDOW` early return and the audio
  device is a second SDL entry point, so every such run overran its recipe silently —
  the symptom is a LONGER successful run, which nothing reports (gotcha 272). Any per-run
  duration quoted from an audio-enabled headless run between phase A/V and part 30 is
  suspect.
* **THE XMA DECODER COSTS NO FRAME TIME.** Three runs an arm, alternated, null first:
  every draw-count bin medians 32.0 ms in both arms, largest mean difference 0.2% against
  a 0.6% null. Bound: the workload is pinned at the two-vblank floor in both arms, so this
  says the decoder does not push frames off the cap. A frame rate quoted from a build with
  audio no longer needs qualifying.

Where the port was, as of 2026-08-11 (part 29):

* **CINEMATICS PLAY TO COMPLETION WITH SOUND — operator-confirmed on two of them, and
  the fix was one field.** `open-items.md` 00j is CLOSED. The XMA packet walk advanced one
  packet at a time; a 2 KB XMA2 packet's header carries **`packet_skip`**, the number of
  packets to step over to reach the next packet OF THE SAME STREAM. That is a no-op for
  mono and stereo — this title's music, SFX and one-shot voice lines, where it is always
  0 — and wrong for 5.1, which the 360 decodes as **several interleaved 2-channel streams
  in one packet stream, one XMA context per pair**. That is why the prologue's contexts 5,
  6 and 7 all pointed at input buffer `02584000`, an oddity this project noticed twice and
  never explained. Each of them was decoding the others' packets: **4.916 s produced from
  a buffer holding 1.845 s, 2.66x**, so the rings filled ~3x faster than the title's mixer
  drained them and the whole voice group wedged after one buffer. Gate on the prologue:
  cinematic-era `runs/distinct` **120 -> 1.00 in every quarter**, the scene's audio clock
  **4.906667 s frozen -> 310.7 s of a 316.5 s track**, `audio/cinematics.big` read
  **2 -> 201** times. A second, separate defect was fixed on the way and is recorded as
  having moved the gate by nothing on its own: the walk retired a spent input buffer by
  unconditionally switching to buffer 1, which this title never uses (136 context dumps,
  `in1Ptr` 0 in every one), parking the context unrecoverably.
  **Both are Case West items on day one**, with gotcha 267's physical addresses.
  The mechanism underneath the symptom was the title's own **`Cine.Audio` PID** on audio
  latency (`sub_824741D8`, shipped mode 2), which returns *setpoint minus an accumulator*
  and so hunts backwards when its input stops. `CZ_CINE_TIME` and `CZ_CINE_AUDIO_MODE`
  remain as the instrument and the arm; the gains are shipped values and were never wrong.
* **The 00j gate was diluted 6x and every recorded reading has it.** `runs/distinct`
  over a whole prologue run reads 6.14 because ~1,870 menu frames contribute 1,010 of
  the 1,170 distinct poses; the cinematic era alone is **38.27** and steady state is
  **15 poses at 120**. `tools/frame_loopiness.py` prints era quarters now. Quote those,
  and read draws beside them — the gate cannot tell a stalled scene from a parked
  player.

Where the port was, as of 2026-08-11 (phase A/V):

* **THE GAME MAKES SOUND, AND THE PROLOGUE CINEMATIC PLAYS.** Both from one fix.
  `docs/phase-av-notes.md`; the plan it executed is `docs/phase-av-plan.md`.
  The XMA decoder is `runtime/audio/xma_decoder.cpp` (ffmpeg, lifted from Fable 2) driven
  by a context walk in `kernel/audio.cpp`; output is `runtime/audio/audio_out.cpp` (SDL).
  **The defect underneath was one address**: the XMA context's buffer pointers are
  PHYSICAL — the APU is a DMA device — and our flat map puts the physical arena in a
  window at 0xA0000000, so reading them literally gave a page of zeros that decodes to
  silence and reproduces the symptom exactly. `NtReadFile ... -> 131072 into A2538000`
  next to `[xma] ctx0 in0=02538000 ... 0 non-zero` is the whole finding (gotcha 267, and
  a Case West item on day one). `maxpeak` 0.000000 -> 0.108854, non-silent 0 -> 15,991
  of 18,433.
  **The prologue cinematic's freeze was that same silence**: same binary, one variable,
  longest frozen camera run **10,513 frames -> 159** and presented coverage **15.00% ->
  99.94%**. `CZ_NO_XMA_DECODE=1` and `CZ_NO_AUDIO_OUT=1` are the two control arms.
  **Part 16 had refuted this with a three-configuration arm and the refutation was wrong**
  — read gotcha 268 before trusting any negative result taken on a stub.

Where the port was, as of 2026-08-10 (phase C part 28):

* **THE ENGINE NARRATES ITSELF NOW, and it cost one byte** (part 28). `CZ_GUEST_DIAG=1`
  clears `0x829EC974` — read by 2,013 diagnostic and assert sites, written by none, and
  shipped as 1. It is a release KILL SWITCH, not the "flag per category a shipped build
  left at zero" that gotcha 215 guessed for thirteen parts (gotcha 266 corrects it). With
  `CZ_GUEST_LOG=1` the outdoor route goes from **0 `[guest]` lines of 11,168 to 1,239**.
  **Reach for it first on any question of the form "what does the title think it is
  doing"** — zone streaming, load timings, heap headroom and the engine's own asserts are
  all behind it. Diagnostic arm only; never quote a frame time from a run with it set.
* **LOD POPS IN LATE — operator report, and it is PARKED BEHIND THE WHITE SURFACES**
  (`open-items.md` 00i). LOD here is STREAMING, not a distance curve: no LOD-distance
  scalar is named anywhere in the executable. The first pass found the per-zone decision
  working (3 zones full, 4 LOD), no streaming failure of any kind, healthy heaps and the
  full 447 MB granted. **The oracle was asked and could not decide it**: hardware's LOD
  swaps are far less visible because hardware's textures are not broken, so while item 00f
  flattens world surfaces to `rgb(180,180,180)` the two arms are not comparable on this
  axis. **Re-ask only after 00f/00g, and do not build the one candidate fix
  (`KeSetBasePriorityThread` is a no-op) before then** — it would target an unproven defect
  and be measured against a picture that cannot report whether it worked.

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
* **CUBE MAPS ARE BOUND AS OF PART 25 — and that is about half the item by volume.**
  Found in part 23, built in part 25. 92 of the cache's 397 shaders sample set 2
  (`TextureCube[]`) and every one of them read descriptor index 0, the 1x1 white dummy, on
  every draw from phase 5 until now. The sidecar now carries each fetch slot's dimension,
  cube maps upload as six faces into a `VK_IMAGE_VIEW_TYPE_CUBE` view in set 2, and
  `CZ_VK_NO_CUBE=1` is the same-binary control arm. **The fetch constant's own dimension
  field was located by CENSUS (`CZ_VK_DIM_CENSUS=1`), not from memory, which had it one
  field off** — dword5 bits 9..10, cross-checked against dword2's stack depth reading 5 for
  every cube fetch (gotcha 244). `docs/open-items.md` item 00 and `phase5-notes.md` §6ay;
  three competing theories died in part 23's census and are recorded there.
  **THE SECOND HALF LANDED IN PART 26: the cube map the title RENDERS ITSELF is now
  assembled from its six resolve snapshots** — `06805000`, six faces at
  `base + i * 0x4000`, copied into a six-layer `VK_IMAGE_VIEW_TYPE_CUBE` image in set 2 and
  refreshed by each face's own resolve (8,850 face refreshes in 240 s, because the title
  re-renders it continuously — a one-shot fill would freeze the world's reflection).
  **The face layout was printed face by face and could have refuted the stride model; six
  of six filled.** `358,767 of 999,508 cube fetches (35.9%)` on the outdoor route now read
  it where all of them read the white dummy before. `CZ_VK_NO_CUBE_SNAPSHOT=1` is the arm.
  **What is still owed is the operator's verdict**, and it is now a three-way question
  (rendered cube / white / no cube at all). Know the headless answer first: part 25's
  four-config block put the magenta positive control at **12.7x its null**, so the
  instrument is NOT blind and binding real cube maps changes nothing measurable in the
  safehouse and prologue — both surviving explanations put the effect OUTDOORS.
* **A CLASS OF WHITE-SURFACE DEFECTS IS OPEN, IT IS OURS, AND SEVEN EXPLANATIONS ARE
  ALREADY REFUTED** (part 26, `docs/open-items.md` 00f/00g). White ground patches, white
  props, blown-out glass and windows — **Xenia renders all of them correctly.** Refuted by
  measurement: the tone map, a missing texture, constant UVs, the white dummy (all four
  heaps poisoned magenta), the clear colour, the EDRAM surface format, a flat-decoding
  texture. **Do not re-buy any of them.**
  Round-2 captures (`Xenia logs/R2_world/`, seven self-contained single-frame traces, read
  with `tools/xtr_draw_bindings.py`) then established: our shader coverage is complete
  (357/357); the ground draw matches hardware on shader, textures, texture CONTENTS and
  render state; and our cube declines fire on a shader-versus-constant disagreement
  hardware never shows.
  **PART 27 CLOSED THE LAST INPUT AND THE ANSWER IS THAT THE GROUND DEFECT IS IN THE
  SHADING.** `tools/xtr_draw_vertices.py` reads hardware's own vertex streams and shader
  constants for a named draw; the 25,234-vertex ground draw agrees on **all five vertex
  attributes to the printed digit** and on every constant the capture can reconstruct. The
  recorded "two texcoords decoding identically" anomaly **is what hardware does too**, and
  item 00f's "drawn twice with mask=F" lead was **TILING** — the two draws' scissors are
  `0,0 640x720` and `640,0 640x720` (gotcha 265). Read our translated
  `ps_ad65b98593f95926` against the capture's disassembly of it next.
  **On the cube declines, part 27 kept the conclusion and replaced the measurement**:
  "414 of 414" counted only slots already reading cube, so it could not have found a
  disagreement (gotcha 264) — `tools/xtr_cube_agreement.py` asks it per declared fetch slot
  and gets 0 of 13,203. The disagreement is ours, it is **9 enumerated (shader, slot,
  texture) cases**, and at those draws our slot 4 holds an **exact duplicate of slot 3**
  where the captures show hardware holding a real 128x128 DXT1 cube map for the same shader
  pair. **The magnitude was off by 10x and had a second, larger cause**: on the outdoor
  route the dummy is served to 3,210 of 1,903,592 cube fetches, **2,182 because the guest
  never set that slot at all** and 1,028 for the disagreement.
  **AND THE `.xtr` TOOLS WERE MISSING `LOAD_ALU_CONSTANT`** — 620 packets against 36
  `SET_CONSTANT`s in one capture, so every constant they reported for hardware was a zero
  that meant nothing (gotcha 262), and 81 of those 620 read memory the trace does not
  carry, which the tools now report as `UNRECOVERABLE` rather than as a stale value
  (gotcha 263).
* **OUTDOOR PICTURE CLAIMS ARE POSSIBLE AS ERA AGGREGATES AND IMPOSSIBLE AS MATCHED
  FRAMES — measured in part 26, and the route was never the problem.** Two arms are
  comparable only where `drawFingerprint` AND `cameraFingerprint` agree; run that filter on
  two runs of ONE configuration on the DebugJump route and it yields **422 of 13,056 frames,
  none above 141 draws, and 0 of the 12,174 outdoor frames** (`tools/frame_determinism.py`).
  The route is fine — 93% of its frames are outdoors and two runs' draw counts agree to
  1.4% — but a crowd of animated actors never renders the same draw list twice, so exact
  equality selects for stasis (gotcha 254). **The replacement is
  `tools/frame_era_medians.py`**: era medians over every frame above 1,800 draws, with the
  null measured from the same pair — **0.94% on mean luma, 0.76% on distinct colours**,
  while coverage saturates at 99.67% and can report nothing. That is what unblocks items
  00, 3, 4 and 6 (`docs/measurement.md`).
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
