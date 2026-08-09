# Port history: what each session established

**Split out of `CLAUDE.md` on 2026-08-08.** The per-session narrative from bootstrap
through phase C part 18, in order. `CLAUDE.md` keeps only the current state; this is the
trail, including the retractions, which is the part that stops a later session
re-deriving something that was already tried and refuted.

The authoritative per-subject records are elsewhere and are not superseded by this:
`docs/xenia-capture-analysis.md` (the numbered findings ledger, which wins on any
measured number), `docs/phase1-notes.md`, `docs/phase3-notes.md`,
`docs/phase5-notes.md` and `docs/d3d-translation-plan.md`.

**Bootstrap + round-1 analysis complete (2026-08-04, session 1).** Package unpacked, XEX
identity established, ladders cross-checked, 232 jump tables recovered, round-1 captures
delivered and analysed, forwards coverage oracle applied.

**Phase 0.1 complete (2026-08-04, session 2).** All 42 unrecognized-instruction sites
closed, plus a seventh mnemonic (`vadduws`) that was "implemented" against a nonexistent
simde intrinsic and could never have compiled. A previously unmeasured defect class —
**dropped direct branches** — was found and driven to zero (finding 13).

**Current image: 57,808 functions, 228 TUs, 155 MB — zero unrecognized instructions, zero
undecodable instructions, zero switch-boundary errors, zero dropped branches, zero
unlowered switch dispatches.** The recompiler log is completely silent. (Was 57,822
before session 5: recovering the two missed jump tables let 14 coverage-injected case
labels and mid-body fragments be removed, which is a correction, not a loss.) Reasoning behind the bootstrap numbers:
`docs/bootstrap-2026-08-04.md`. Behind the capture-derived ones:
`docs/xenia-capture-analysis.md`.

The pipeline is now five tools that **must run in this order**, each re-running the
recompiler in between, because each one's evidence is only valid against a current `ppc/`:

```
find_jumptables.py  ->  coverage_to_function_overrides.py  ->
    fix_switch_function_bounds.py --apply  ->  find_dropped_branches.py --prune / --widen
    ->  find_unlowered_switches.py
```

The last one is a **gate, not a repair**: it asks the image which `bctr` sites look like
a table dispatch and are missing from the switch TOML, which is the only question
`find_jumptables.py`'s own output cannot answer (gotcha 53). Exit 1 means a real defect.

**Phase 0.2 complete (2026-08-04, session 2).** `runtime/` exists. All 228 TUs compile
and link — **0 errors, 0 warnings, 89 s on 16 cores** → 155 MB `libppc_image.a`, 109 MB
`cz_smoke`. The gate binary walks all 58,303 `PPCFuncMappings` entries and validates
them; the binary contains all 57,822 guest functions and all 244 imports with zero
undefined symbols.

**Phase 0.3 complete (2026-08-04, session 3): the `.xtr` decoder exists and finding 10 is
closed.** `tools/xtr.py` (the format, in one module) plus `xtr_walk.py`,
`xtr_pm4_census.py` and `xtr_determinism.py`. Format and method: `docs/xtr-decoder.md`.

Measured: B1 and B1b are **content-deterministic to 0.42%** over the boot+movie prefix
(0.19% on draws), with four eras agreeing to the individual draw — but **frame-exact
agreement is only 80.0%**, so phase 4 must gate on per-era aggregates, never on frame
index (gotcha 38). Both captures are intact: clean heads, zero desyncs. The census
self-check found `INDIRECT_BUFFER` is recorded one dword short (gotcha 39), which is a
trap phase 4 would otherwise have hit at replay time.

**PHASE 0 IS COMPLETE.**

**Phase 1 in progress (2026-08-04, session 4): the guest boots, and the GPU is real.**
`docs/phase1-notes.md` is the record; read it before continuing.

The recompiled image runs under our runtime, brings up TLS, threads and the loader
seam, reads files off the package, and **drives a live PM4 command processor**. Over a
25 s run: 1.27 M packets parsed, 563 XE_SWAP frames, 68,588 draws, 1,235
command-processor interrupts delivered to the guest ISR — with **zero unknown opcodes,
zero parser stalls and zero out-of-arena stores**, and the read pointer chasing the
write pointer rather than frozen.

- **`--xenia A1` (masked): an exact 84-deep prefix of Xenia's 93**, stopping before
  `XamShowDeviceSelectorUI`. Position 84 is `MmMapIoSpace`, the XMA context mapping.
- **`--xenia A5 --include-high-frequency`: tracks A5 to position 119, its last, with
  ZERO real mismatch windows** — `SET MATCH: every mismatch is a permutation. Exit 0.`
  The two surviving windows are permutations of one name set each, i.e. thread
  scheduling (findings 35-36). First fully clean A5 gate this port has produced.
  **Gate at 90 s, not 30 s**: at 30 s the run stops around 114 and the XMA path looks
  blocked when it is merely slow (gotcha 75). Even at 90-150 s reaching position 84 is
  usual, not guaranteed — 5 of 7 runs this session — because how far a multi-threaded
  boot gets in fixed wall time is a distribution, not a fact.
- 155 of 244 imports real, 89 generated honest-failure stubs.
- `cz_runtime --smoke` still passes: the phase 0.2 link gate is intact.
- **Stability: 0 crashes in 8 runs at 25 s, and 0 in 8 with `CZ_NO_AUDIO_PUMP=1`** —
  the audio pump's own control arm, same binary, same session. Read it as "no
  measurable difference", **not** "the pump is safe": 8 runs cannot see a 1-in-20
  fault, and the known surviving crash is around that rate (gotchas 50-51). The
  dominant fault — the "null-pointer walk on the main thread", 6-7 in 10 — was the
  unlowered `bctr` of finding 27 and is gone; the poison indirect call on the
  *graphics* pump thread is declined. `runtime/cpu/crash_report.cpp` prints the guest
  state on any fault.

Three things from this session worth carrying to Case West, all in
`docs/phase1-notes.md`:

- **The async completion is an APC, not an event** (finding 20). Our `NtReadFile`
  filled the buffer, filled the `IO_STATUS_BLOCK`, returned success — and hung the
  boot, because the engine passes `event = 0` and signals through `apcRoutine`.
  Every observable looked correct. Only A5 shows it; `NtReadFile` is `kHighFrequency`.
- **The ring geometry is derivable from the guest's own arithmetic** (findings 22-23).
  `28 - clz(size)` in front of the call proves `size = 1 << (arg + 3)`, and the single
  `CP_RB_WPTR` store gives the device-struct offsets. No constant needed inheriting.
- **Xenia's physical addresses carry a +0x1000 skew** (finding 24), so a physical
  address in our log is 0x1000 below the same object's in a capture. Any geometry
  argument mixing the two conventions is wrong — this one briefly manufactured a
  ring-buffer overrun that did not exist.

**The dominant crash is fixed (2026-08-04, session 5): it was not a null pointer.**
`docs/phase1-notes.md` finding 27 and `docs/xenia-capture-analysis.md` §15. A `bctr`
inside `sub_82955780` had never been lowered to a `switch`, so the function returned
without its epilogue and its caller resumed with the callee's `r31`. Finding 26's
diagnosis ("something we return is null where an object is expected") is retracted —
no kernel call was involved. Two such sites existed image-wide; both are fixed, the
image now carries **234 switch tables (was 232)**, and `find_unlowered_switches.py`
reports **0 defects, 2 benign tail-call thunks**.

Measured: **6-7 crashes in 10 runs -> 1 in 20.** The pipeline is clean end to end —
silent recompiler, zero dropped branches, zero `// ERROR`, `--smoke` passing.

**The frontend was waiting for input, and that is now measured (session 6).**
`docs/phase1-notes.md` finding 37. A healthy run reaches the **title screen** — 64
files through to `prologue_menu\prologue_z01.big` — the print cap, corrected in part 6
to 84 files ending at `skeleton\cinezombie.big` (gotcha 109) — rendering at ~34 fps and ~1,982
draws/frame against A1's title-screen ~2,540 — and sits there. Supplying one synthetic
START press (`CZ_FAKE_START_MS`, a measurement arm, never on for a gate) advances the
A1 gate from an 84-deep prefix to **85**, `XamShowDeviceSelectorUI`, five log lines
after the press. 2 of 2 conclusive pairs; the runs that reached the title screen
without a press did not advance. It then stops at position 86 on the phase 2
save-data enumerate stubs — a gap we chose.

Two things fell out. The standing note ("reaches `mainmenu.tex` and stops") had gone
stale and would have sent someone hunting a file-loading bug that no longer exists
(gotcha 79). And about a third to a half of long runs never reach the title screen at
all, stalling with the main thread parked in the renderer's frame fence — a real
defect, traced end to end in finding 38 below.

**The load stall is traced end to end (session 7) and FIXED (session 8) — and it was
our own kernel, not the parser.** `docs/phase1-notes.md` findings 38 and 39.

The chain finding 38 established: the main thread waits on the render fence; the
**Draw Thread** is in no kernel wait at all, spinning in guest code (`sub_8283C6C8`
under `sub_82845160`) on a ring-progress counter; that counter is advanced only by
`EVENT_WRITE_SHD` fence packets in the title's own command stream; and our walk of the
indirect buffers those fences close **stops early**, silently, on data it reads as a
packet header. Drop the last packet and a thread waits for the life of the process.

Finding 39 is why the walk desynced, and it is nowhere near `gpu/pm4.cpp`. **`VdSwap`
wrote 12 dwords of a 64-dword reservation and left the other 52 alone.** The caller
advances its write pointer by the whole reservation regardless (`addi r11,r29,256`,
`r3` never read), so those 52 dwords were submitted to the command processor as
packets — and since command buffers are recycled, what was in them was the *previous
frame's command stream*. Hardware fills them with 52 × `0x80000000`, a type-2 one-dword
no-op; the count is confirmed by two independent witnesses, the guest's own `addi` and
B1's 43 swap-carrying indirect buffers (52 every time).

Measured, same binary, arms alternated, `CZ_NO_SWAP_PAD=1` as the control:
**truncated indirect buffers per 120 s run went from up to 2,945 to ZERO**, and the
stall with them.

Three method notes worth more than the fix:
- Two capture-derived gates said the parser was correct — packet lengths against
  hardware's boundaries on 24,527,474 packets, and every indirect buffer's start
  address and internal boundaries chained on all 28,726
  (`tools/pm4_indirect_walks.py`, written this session). Both were right. An oracle
  for your arithmetic does not clear your inputs (gotcha 88).
- The timing theory — "our vblank-driven command processor reads buffers a frame late,
  so the guest is writing under us" — was plausible, explained the symptom, and would
  have meant rewriting the command processor. `CZ_PM4_IB_VERIFY=1` killed it for the
  cost of a memcpy: **84,808 buffers walked, 0 modified.**
- The desync was located by building a vocabulary from the capture — B1 uses only
  **225 distinct packet headers** in 24.5 M packets — and flagging the first header in
  a dumped buffer that is not in it. All six pointed at the same place, immediately
  after `XE_SWAP`, after six truncation reports had each named a different innocent
  dword (gotchas 85, 89).

Finding 38's zero-header story is retracted a second time: B1's single zero-header
packet is a mis-recorded `INDIRECT_BUFFER`, so the capture contains **no** genuine one
and never had an opinion. `CZ_PM4_ZERO_IS_NOP` stays as an arm and is no longer
interesting.

**The 1-in-40 crash does not reproduce, and hunting it found a memory-barrier hole
(session 8).** `docs/phase1-notes.md` finding 40. Re-measured first, as an inherited
rate always must be: **0 crashes in 20 runs at 120 s**, all 20 reaching the title
screen. The old figure predates finding 39, when a third to a half of runs stalled in
the first minute. Not "improved" — unmeasurable at this sample size, and saying which
would take hundreds of runs nobody needs yet.

What the site means is now understood: thread `0xF2C` (and `0xF30` — there are two)
runs the graphics driver's command-stream consumer, and the null is a callback slot
that only a `0x8C000000` token in the guest's own token stream ever sets. A null there
means the interpreter ran a "run" token before any token set a callback, i.e. it walked
a stream that had not been published — a producer/consumer ordering question.

Which is how **XenonRecomp bug 6** turned up: `sync`, `lwsync` and `eieio` all lowered
to `// no op`. Right for the hardware half on x86-64 and wrong overall, because every
guest access is a plain C++ load/store through `base` and a construct emitting no code
constrains clang not at all (gotcha 92). Now `lwsync`/`eieio` -> `atomic_signal_fence`,
`sync` -> `atomic_thread_fence`. **This is NOT credited with fixing the crash** — the
baseline was already 0 of 20, so there was nothing to improve on; it is a correctness
fix, measured only for absence of regression.

Two instruments came out of it: the crash reporter now names `ctr == 0` (its old test
required `si_addr == nullptr`, which is never true for that case — it was silent on the
one shape it existed for), with `CZ_CRASH_TEST=nullcall` to prove it; and
`tools/gdis.py`, the guest disassembler, kept on the third time of writing it.

**The audio driver is real and the A5 gate is clean (2026-08-04, session 6).**
`docs/phase1-notes.md` finding 36. All seven audio imports implemented: an XAudio
render-driver client with a guest-thread pump at 5.333 ms/frame (256 samples x 6
channels, planar f32 — read out of the title's own de-interleave loop), and the XMA
context array published into the decoder's little-endian MMIO register file at
`0x7FEA1800`. The deciding detail was that the registered callback is
`lwz r3,0(r3); b <body>` — **the driver passes a POINTER to the context, not the
context** (gotcha 72). Our `MmMapIoSpace(bus=2, phys=..., 64 bytes, protect=404)`
now matches A1's field for field.

Two general defects fell out of it, neither audio-specific: `WaitDispatcher`
dereferenced a guest pointer unguarded, so any null dispatcher object crashed the
host inside our own kernel (gotcha 73); and a runtime-owned physical allocation was
displacing the title's own 447 MB reservation (gotcha 74).

**Two of sixteen cores were being burned by a polite-looking spin (session 9).**
`docs/phase1-notes.md` finding 41. Case Zero blocks two threads forever by design —
`DnsLookupThread` and the session shutdown thread each enter a critical section the
main thread never releases — and on console they simply sleep. Our
`RtlEnterCriticalSection` spun on `std::this_thread::yield()`, which on an idle
multicore host returns immediately, so each of them burned a core for the whole run.

Contended sections now `pause`-spin (64), then yield (256), then **park on a host
condition variable** signalled by `RtlLeaveCriticalSection` — a park rather than the
originally-proposed 1 ms sleep, because a fixed sleep would charge the quantum to every
section held longer than the yield phase. The release path needs a `seq_cst` fence: it
stores the lock word and then loads the waiter count, and store-then-load-elsewhere is
the one ordering x86 does not give (gotcha 92 again, one session later).

Measured, same binary, arms alternated, `CZ_CS_NO_BACKOFF=1` as the control:
**317% → 121% CPU, system time 85 s → 4.5 s, frames unchanged at 1943 ± 1.** The
latency question is settled by `CZ_CS_STATS=1` rather than argued: of 1.6 M
acquisitions in 60 s, 0.26% are contended at all, 417 outlive the pause phase, and
**2 ever reach the park phase**. Gates unchanged — A5 exit 0, A1's full 84-deep prefix,
`truncated=0`, and position 71 permutes 1-of-3 on *both* arms.

**Phase 3 is built (2026-08-05, session 10): there is a window, a present seam and a
real pad.** `docs/phase3-notes.md` is the record.

`runtime/host/window.{h,cpp}` — one module, because in SDL a window, a present and an
input device are one thread. The guest entry moved to a spawned thread so the main
thread can own SDL (gotcha 99); the present is driven from `pm4.cpp` case `0x64`, i.e.
from where the command processor *reaches* the swap packet, not from `VdSwap` (gotcha
100); and `XamInputGetState` now answers out of a real keyboard and, when one is
attached, a real SDL game controller.

**A blank window is the correct result of this phase** — there is no renderer until
phase 5. The title bar carries the live frame count, which is what says the present
seam is running.

Measured with the arms alternated over six 100 s runs, old binary rebuilt and run
*now* (gotcha 86): both arms reach A1's full 84-deep prefix, A5 exit 0, `truncated=0`,
title screen 3 of 3. The window costs ~1% of the frame rate — 3151 vs 3183 frames —
and `CZ_NO_WINDOW=1` on the *phase 3* binary returns exactly 3183, so the cost is the
window rather than the wiring. **Position 71 permutes on BOTH binaries** (3 of 3 on
the old one), which is the same scheduling-sensitive window findings 41 and gotcha 86
already recorded — not a regression, and 1-of-3 vs 3-of-3 is not an improvement
either.

**THE PHASE 3 GATE PASSED, on one real press.** The operator focused the window and
pressed Enter; `[host] pad packet 2: buttons=0010` (START), and **five log lines later
`XamShowDeviceSelectorUI` — position 85** — before the key was even released.
`CZ_FAKE_START_MS` appears zero times in that log. Every run before it stopped at 84.
The gate could not be self-served — any press this machine could synthesise is the arm
the gate exists to retire — so it was scheduled with the operator like a capture
(gotcha 103), with everything not depending on it committed first. Three of the four
arms are now observed: real press → 85; no press over 420 s → 84, zero pad packets;
`CZ_NO_WINDOW=1` → 84. Two packets for one press over ~600 polls, which is gotcha 101's
contract holding.

**The save-data layer is built, and the A1 gate now reaches 92 of 93 with no
divergence at all** (2026-08-05, same session) — `docs/phase3-notes.md` §9.
`runtime/kernel/content.cpp` implements the content enumerators, the XAM enumerate
message and the `save:` mount, and the whole protocol was recovered from the title's
own statically-linked `XamEnumerate` rather than from a capture, which cannot show a
return value (gotcha 104). The chain lands in A1's exact order and our
`XMsgCompleteIORequest(result=1627, extended=80070012, length=0)` matches A1's line
field for field. Four of finding 34's never-executed imports executed for the first
time, `XamTaskSchedule` among them.

The defect worth remembering is gotcha 105: `XamGetExecutionId` was a stub, and it is
the enumeration's **title-id filter** — so every save this runtime enumerated was
silently skipped, producing a log identical to an empty save list. Measured, one save
present: title-id field 0 → filtered (`result=1627`), title-id field `XexTitleId()` →
**accepted (`result=0`)**.

The press also showed where the boot goes next, and it is exactly where the plan said:
after `XamShowDeviceSelectorUI` the title resolves a XAM export dynamically
(`XexGetProcedureAddress(xam, ord=0x279)` — **A1 makes the same call four lines after
its own**, so this is the sequence, not a divergence) and then runs
`XamContentAggregateCreateEnumerator` → `XamGetPrivateEnumStructureFromHandle` →
`XamAlloc` → `XamTaskSchedule` → `XamGetOverlappedResult` → `XMsgInProcessCall` →
`XMsgCompleteIORequest`. That is positions 86-92 and **precisely the save-data layer
deferred out of finding 34** — and it includes `XamTaskSchedule`, one of the eight
implemented-but-never-executed imports, so that debt starts being paid by the next
phase rather than needing its own.

**PHASE 5 IS BUILT: there is a renderer, and it draws real game content
(2026-08-05, session 11).** `docs/phase5-notes.md` is the record — read it before
touching `runtime/gpu/vk_renderer.cpp`.

`runtime/gpu/vk_renderer.cpp` translates the PM4 draw stream onto a host Vulkan device
with the XenosRecomp shaders. **Off unless `CZ_VKDRAW=1`**, which makes the phase 3
binary available in the same build as the control arm for every claim below.

- **The shader pipeline is complete: 336 of 336 distinct shaders translate, zero
  failures**, and not one recompiler change was needed — XenosRecomp's Fable 2 patches
  carry over whole. `tools/build_shader_spv.sh` is the pipeline.
- **Our `IM_LOAD` arithmetic is now validated against hardware.** A boot dumps 121
  distinct microcode blobs and **120 are byte-identical to A1's**, modulo dword order.
  Nothing had ever checked that packet's size field.
- Measured over a 120 s headless boot: **1,087,826 indexed draws**, 125 pipelines, 958
  textures untiled and uploaded, 67 resolve snapshots, **450,488 texture fetches served
  from a snapshot**, and **1,187 of 1,195 frames presented from the front-buffer
  resolve**. The picture is the blood streak from the DEAD RISING 2 wordmark plus UI
  text — recognisably E2's logo, and not yet all of it.
- **All pre-existing gates hold with the renderer on**: `--smoke` OK, A1's full 84-deep
  prefix, A5 exit 0 (2 windows, both permutations), both PM4 capture oracles clean,
  `truncated=0`, zero parser stalls. Position 71 permuted on the renderer-**off** arm
  this time, which is gotcha 86's lesson arriving from the other direction.
- **Cost: 1,488 frames per 100 s with the renderer on against 3,090 with it off** —
  roughly half the frame rate, from a synchronous submit and a full readback per frame.
  A number to re-measure once the picture is right, not a defect to fix before it.

**ARCHITECTURE PIVOT DECIDED (2026-08-06, operator's call): the renderer moves to a
D3D TRANSLATION LAYER in UnleashedRecomp's architecture** — hook the title's
statically-linked XDK D3D functions, never touch the ring. **THE NEXT SESSION STARTS
FROM `docs/d3d-kickoff.md`** — it carries the hand-off, the measured recon tables
(Present is identified: `sub_82841F00` is `D3DDevice_Swap`, the 7 ring-emit
primitives are named, device init is two walks from CreateDevice, all 20 Vd-import
anchors listed), the OBSERVE-then-REPLACE bring-up order, and the missed-hook
detector (`Pm4_PacketCount()==0` on the hooked arm). `docs/d3d-translation-plan.md`
is the decision and licensing record beneath it. **plume is license-VERIFIED MIT**;
only video.cpp-derived code carries GPLv3. The short form: every hard renderer defect this phase hit lived below the D3D
line, and UnleashedRecomp's runtime stubs VdSwap/VdInitializeRingBuffer EMPTY — the
whole findings-38-41 layer is dead code in that architecture. Case Zero's D3D cluster
is already bounded (0x8283xxxx-0x8284xxxx, TUs 159/160/175/176) via import call sites;
the PM4 executor stays as boot engine and control arm; the shader cache, texture
decode, and every instrument transfer. UnleashedRecomp is GPLv3 and the operator has
authorized taking code, not just structure — provenance headers on every adapted file.

**PHASE A IS DELIVERED (2026-08-06, session 12): the hook table exists and OBSERVE
mode validates it.** The table with evidence per row is in
`docs/d3d-translation-plan.md`; the instrument is `runtime/gpu/d3d_hooks.cpp`
(`CZ_D3D_OBSERVE=1`, 43 hooks, log + call through). The structural result that
shrinks Phase B: of the cluster's 117 externally-called functions only **27 can
reach the ring** — the rest are state setters writing the device struct's register
shadow and can stay guest code even in replace mode. CreateDevice is
`sub_8283CCE8`; the engine submits draws directly (finding 40's worker threads are
idle at boot/title); a 360 Clear is a resolve-with-clear-bits, which reconciles the
API stream's 20 Clears/frame with phase 5's ~20 resolves/frame.
`tools/guest_callers.py` is the call-graph scanner Phase A was answered with —
reach for it before disassembling anything's callers by hand.

**PHASE C IS BUILT AND RENDERING (2026-08-06, session 13): draws serviced by
REDIRECTED EMISSION — the title's own flush is the encoder.** `CZ_D3D_DRAW=1`
redirects each content API call's command-buffer cursor (`dev+0x30/0x34/0x38`)
into a private guest scratch, lets the guest body run, and a private PM4-subset
walker (`runtime/gpu/d3d_draw.cpp`) folds the emission into a private register
file + shader hashes for the phase-5 renderer's decode guts
(`VkRenderer_D3DDraw/D3DSwap`, parameterized — `pm4.cpp` untouched as the control
arm). Measured: the legal screen and CAPCOM logo render pixel-correct from the
D3D arm; **A1 = exact 82-prefix (phase B's KeResetEvent window CLOSED), A5 exit 0
with zero real windows — the port's best kernel gates**; zero crashes on the
final interrupt design. The hard-won piece was interrupt delivery: content-stream
INTERRUPTs carry the token worker's kick, their arms are dual-transport, and the
walker now performs the ISR's source-1 path itself from one guarded read (four
designs; the trail is in the git log and `docs/d3d-translation-plan.md`).
~~OPEN BLOCKER: the boot deadlocks mid-cinematics.~~ **CLOSED in session 14, below** —
and the walker's ISR replication described above is retracted with it.

**PHASE C PART 2 (2026-08-06, session 14): the movie deadlock is fixed, and the rule
the redirect was missing is "emit where the READER lives".** Details in
`docs/d3d-translation-plan.md` §"Phase C part 2".

`sub_82846288` is the **callback armer** (the Phase A "fence/throttle" label is
retracted): it lays down an arm of scratch registers `0x057C/0x057D`, three
`WAIT_REG_MEM`s, an `INTERRUPT` and a re-poison, and the graphics ISR reads that
callback back out of GUEST MEMORY. Redirected emission put the whole block in our
private scratch, where the walker had to emulate a hand-off whose correctness IS its
ordering — four designs, all racing the poison. It now runs with the REAL cursor
restored (`D3dDraw_ServiceRealRing`), so the title's own ISR delivers it. The second
half: **the reserve `sub_82845F68` is not "give me space", it is CLOSE-AND-KICK**, and
Resolve's multi-tile path calls it purely for the kick and discards the return value;
suppressing it left the block unkicked.

Measured, same binary, one boot each — and both halves are load-bearing, because
`CZ_D3D_NO_RESERVE_KICK=1` reproduces the old stall exactly:

| | before | after | NO_RESERVE_KICK arm |
|---|---|---|---|
| ISR delivers `sub_8284AAD0` (the worker kick) | **1 in a whole boot** | continuous* | 5 |
| walker-delivered interrupts | 200, all `82841878` | 0 | — |
| deepest file | #56 `cinematics.big` | **#60 `models\zombies.big`** | #56 |

Gates on this binary, both arms: `--smoke` OK; A1 = exact 84-prefix (control) / exact
82-prefix (draw); A5 = exit 0, 2 permutation windows, **0 real**, on both. Unchanged
from the phase C best.

~~**THE NEW BLOCKER, localised:** the boot parks at `models\zombies.big` with the
engine thread at 99% CPU in `sub_82846210`'s `while ([dev+0x2B04] != 0)` spin ... the
worker is woken constantly with nothing to drain; reconciling that is probably the
fix.~~ **Half retracted in session 15 — see phase C part 3 below. The spin is real; the
reading of it was not.** The counter is **NEGATIVE**, so the `!= 0` test can never
succeed, and the worker drains far MORE than the title submits (6 increments against
18,900 decrements in one boot), not less.

**PHASE C PART 3 (2026-08-06, session 15): the counter is negative because the command
processor is REPLAYING the hand-off block.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 3"; hand-off in
`docs/d3d-phase-c4-kickoff.md` (superseded by
`docs/d3d-phase-c5-kickoff.md`).

`CZ_PM4_MEM_WATCH` pointed at the ISR mirror's callback slot counts **8,152,069 writes
in 200 s** — `8284AAD0` armed 2,717,263 times and poisoned 4,076,035 — while the guest
calls the armer **405 times**. Corroborated three ways: the ring goes from ~390 packets
and ~48 draws a frame to **1.25 M packets and 135,000 draws per second** with `XE_SWAP`
frozen; `sub_828455C0` is called **106 M times**, always `count=1`, always cycling the
same three segments (93/11/23 dwords, the 93 being the one that contains the arm); and
the fence-completion word freezes at a constant while `emitted` climbs, which is the
signature of replay rather than of a slow GPU. `truncated=0` and the IB verify stay
clean throughout — the parser is right and the bytes are wrong (gotcha 88, third time).

The loop needs no seed and has gain one: a segment containing an arm block reaches the
worker's token stream -> the worker submits it -> the CP executes the arm and its
INTERRUPT -> the ISR's `sub_8284AAD0` pushes the SAME token-buffer pointer back on the
worker's ring -> the worker restarts that buffer at `buffer+4` -> resubmits the segment.

Two more of the four arm-block emitters moved to the real ring this session, both on
measured evidence and **neither of them the cure**: `sub_82841AD0` (the Phase A name
"PreSwapResolve" is retracted — it RESOLVES NOTHING, it is a pure GPU/CPU hand-off
emitter, and redirected it put all 405 of a boot's armings in the private scratch), and
`sub_8284B9C0` (all six of its calls ran with the scratch cursor installed; it is also
the only site that arms `sub_8284AAD0` and the only `+1` the counter gets).
`CZ_D3D_REDIRECT_PRESWAP=1` is the same-binary pre-fix arm.

~~`CZ_PM4_STOP_ON_WAIT=1` was re-tested rather than inherited as retired: part 2 retired
it while the arm blocks were in the SCRATCH, where the flag could not apply to them at
all. With them in the ring it genuinely gates them — and it is still runaway.~~
**Retracted in part 4 below: the flag was gated on `depth == 0`, so it could not apply
to these packets in EITHER session** (gotcha 151).

Gates, this binary, both arms: `--smoke` OK; A1 = exact 84-prefix (control) / exact
81-prefix (draw, when the run does not hit the long-known position-71 permutation);
A5 = exit 0, **0 real windows**, on both. Unchanged from the phase C best.

**PHASE C PART 4 (2026-08-06, session 16): the replay is the FLYWHEEL, not the fault —
and the one brake our command processor has never had is now built.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 4".

Part 3's two ranked candidates are both retired **by measurement, on the control arm**.
The arm block is inside its own segment by construction (`sub_8284B9C0` writes it at
`r28-4` and submits `[r28, armEnd+4)`), and the control arm queues that segment to the
worker on 5,696 of its 5,698 frames without ever looping — so "the queued segment
should not contain the arm block" was never a difference between the arms. The same
probe on both arms, same era:

| | PM4 control | phase C draw |
|---|---|---|
| `[fence] arm cb=8284AAD0` : `[fence] isr cb=8284AAD0` | 768 : 766 | 12 : 856 |
| whole boot, `incr=1` submits : drains | 3,958 : 7,913 | 6 : 132,545 |

The hand-off regenerates its own wake-up on hardware too, and it converges because the
guest arms with a NEW token buffer every frame: gain one, pointer advancing, a
pipeline. The draw arm's whole boot has **four** armings cycling between **two** buffer
pointers, so once the guest stalls each walk resubmits the same segments forever.
**The guest stalling makes the counter negative and the negative counter keeps the guest
stalled** — part 3 read the flywheel as the cause.

The missing brake: on hardware the CP STALLS at the hand-off block's `WAIT_REG_MEM`s.
`gpu/pm4.cpp` gated `CZ_PM4_STOP_ON_WAIT` on `depth == 0`, and every one of these waits
is inside an INDIRECT BUFFER — so both of the flag's retirements measured a no-op
(gotcha 151). It now stalls at any depth and **resumes at the recorded dword** rather
than re-walking the buffer (gotcha 152), and a deliberate stop is explicitly not a
truncation. With it working, **both arms park at frame 1 on the same packet**: a wait
for SCRATCH_REG1 (`mirror+4`, register `0x0579`, the one `sub_82841AD0` sets to 1) to
read back zero, which nothing in our runtime ever writes. That is a statement about the
runtime, not about phase C, and it is where part 5 starts.

Gates, this binary, both arms, default flags: `--smoke` OK; A1 = 84-prefix on the
control arm (this run hit the long-known position-71 permutation) / **exact 82-prefix**
on the draw arm; A5 = exit 0, **0 real windows**, on both; `truncated=0` with 3.59 M
packets walked. Deepest file: #63 `prologue_z01.big` (control) / #60 `models\zombies.big`
(draw). Unchanged from the phase C best.

**PHASE C PART 5 (2026-08-06, session 17): the missing CPU side of the hand-off was a
DISPLAY CONTROLLER, and the brake now works.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 5"; hand-off in
`docs/d3d-phase-c6-kickoff.md`.

Part 4's question — who writes the zero the hand-off block's `WAIT_REG_MEM` waits for —
is answered, and the answer is nowhere near the ring. `CZ_PM4_MEM_WATCH=BBF39464` on a
healthy control boot: **3,089 writes, every one the value 1, every one from the PM4
stream.** The zero comes from the CPU, in the guest's own vblank ISR path, behind a GPU
MMIO **read** at `0x7FC86544` bit 0 — the display controller's gate, which our runtime
left at zero for the life of every process this port has ever started. Behind it sits
the title's swap queue: 16 records of `{surface, due tick}` at `dev+0x418C`, a vblank
tick at `dev+0x4174`, and a walker (`sub_82841760`) whose ONLY caller is that branch.
A record whose surface is zero means "nothing to scan out, just release the GPU" and IS
the `[mirror+4] = 0` store. Fable 2 found the same gate at the same address (its
findings 48 and 57); two sessions of phase C reasoned about the hand-off's packets
without either port's notes being consulted for the register (gotchas 153-154).

A second defect surfaced the instant the first was fixed: **the graphics interrupt is
addressed to a SET of hardware threads.** The arm block writes a six-bit CPU mask into
`mirror+0`, the ISR clears `1 << PCR[0x10C]`, and the block's trailing `WAIT_REG_MEM`
holds the CP until the word is zero. The mask DEFAULTS to CPU 2 — where the pump has run
since phase 1, so the common case was right by accident — but `sub_827D2FC0` arms with
CPU 4 and our one ISR thread could never acknowledge it. The pump now takes the interrupt
once per named CPU (gotcha 155). The ISR body is per-CPU too: `sub_8284AAD0` pushes onto
the job ring at `dev + cpu*0x6C + 0x2C40`.

Measured, same binary, arms alternated, one 100 s boot each:

| | control (`CZ_NO_VBLANK_GATE` / `CZ_ISR_SINGLE_CPU`) | fixed |
|---|---|---|
| vblank tick after 30 s | 0 | 1,860 (62/s) |
| swap queue, brake OFF | head 0 / tail 1,540 | head 27 / tail 1,165 |
| brake ON, PM4 arm | parks at frame **7**, `ack=00000010` | **217 frames and climbing**, head = tail, `truncated=0`, `prologue_z01.big` |
| brake ON, draw arm | parks the same way | **725 frames**, head 724 / tail 725, `truncated=0`, `models\zombies.big` |

**With all three pieces — the gate, the per-CPU acknowledge and part 4's
stall-with-resume — `CZ_PM4_STOP_ON_WAIT=1` runs this title's real GPU/CPU hand-off end
to end, paced by the guest, on BOTH arms.** The draw arm's part-3 runaway (1.25 M packets
and ~135,000 draws a second with `XE_SWAP` frozen) does not happen: 306,288 packets,
46,560 draws, 725 frames. The gate and the per-CPU acknowledge are ON by default; the
BRAKE is not, because this is one run per arm and not a rate (gotchas 50-51) — measuring
it properly and promoting it is part 6's first job.

The cost, stated because it is real: **with the brake OFF the per-CPU acknowledge makes
the draw arm's runaway spin harder** (1,745 -> 2,856,448 `XE_SWAP` in 100 s), because
each interrupt now produces several worker kicks instead of one. Same flywheel, more
gain; the control arm is untouched (3,091 vs 3,088 frames). It stays default-on because
it is correct, and the pairing with the brake is now explicit.

Gates, this binary, default flags: `--smoke` OK; A1 = **exact 84-prefix** (control) /
position-71 divergence (draw, the long-known scheduling window); A5 = **exit 0, 0 real
windows, on both**. Deepest file: `prologue_z01.big` (control) / `models\zombies.big`
(draw). Unchanged from the phase C best.

**PHASE B IS DELIVERED (2026-08-06, session 12): `CZ_D3D=1` services the content APIs
(draws/clears/resolves → no-op) while the frame lifecycle calls through — and
the ring goes SILENT (+0 packets/frame steady state), the boot reaching the
title screen at ~340 fps with zero faults over 33,984 frames.** The title's own
Swap takes its empty-frame branch when nothing was drawn, so the completion
protocol did not need replacing for the skeleton. Two failures worth their
weight: servicing Swap directly deadlocks three threads (the completion protocol
lives in the D3D worker `sub_8284B828` + an event inside the device struct), and
servicing the busy-track entry `sub_82837D70` crashes — it RETURNS A CPU POINTER
(a Lock-style API); OBSERVE validates firing patterns but only REPLACE validates
return-value semantics. A1 on the replace arm has exactly one real window
(`KeResetEvent` + the ISR spinlocks — all verified downstream of ring
consumption, which the arm removes by design). Next: phase C — service the
draws/state with a host renderer reusing `vk_renderer.cpp`'s decode guts, keyed
off the device struct's register shadow (offsets in the Phase A table).

**PHASE C PART 6 (2026-08-06, session 18): the brake is the DEFAULT, on 40 runs — and
three of the four numbers it was to be judged on could not have judged it.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 6".

`CZ_PM4_STOP_ON_WAIT` is promoted; **`CZ_PM4_NO_STOP_ON_WAIT=1` is now the control
arm.** 10 runs per configuration, 120 s each, arms alternated within each round:

| | frames (median) | spread | max hold streak | queue head==tail | deepest | crashes |
|---|---|---|---|---|---|---|
| PM4, brake off | 3,680 | 1x | 0 | **0 of 10** | #83 | 0 |
| **PM4, brake on** | 2,446 | 1x | 1 | **10 of 10** | #83 | 0 |
| draw, brake off | 290,874 | **10,397x** | 0 | 3 of 10 | #60 | 0 |
| **draw, brake on** | 3,616 | 1x | 2 | **10 of 10** | #60 | 0 |

`truncated=0` in all 40. The cost is 2,446 frames against 3,680 and it is not a loss —
it is the title paced at its own frame timing rather than the CP outrunning it. Two
facts in that table are about the title, not the brake: **free-running overflows the
flip queue in 10 of 10** (the unpaced state was never healthy, it just had no
instrument on it), and **the draw arm's default configuration is BIMODAL** — 332 to
3,451,841 frames — so part 5's two draw-arm numbers are two modes of one distribution
(gotcha 159).

**Part 4's prediction is RETIRED, not confirmed.** Re-running part 3's instruments on
the current binary: the brake cuts the callback hand-off's per-frame amplification
~39x (430 -> 11.1 kicks/frame against the control arm's 1.9) but leaves the raw
deliveries-to-armings ratio **unchanged at ~300x**. It contains the symptom; the cause
is untouched, and that is where part 7 starts. (**The ~300x is RETRACTED by part 7** —
it is a stopwatch, not a gain; see below. The ~39x per-frame figure stands.)

Three of the harness's own numbers were broken before any of the above could be
measured, and the story is gotchas 157-160: the deepest-file column was the PRINT CAP
(the boot opens 84 files, not 64, and ends at `cinezombie.big`); the stall counts were
the running index of a capped print; the number that decides the promotion — is a stall
ever released — did not exist; and the counter written for it was wrong twice, caught
both times by the deliberately-parked control arm rather than by reading the code.
`MirrorIsPoisoned()` is NOT retired by the promotion as the kickoff expected: it
records zero skips across all 40 runs **including brake-off**, so it was already inert
independently.

**Gates:** `--smoke` OK; A5 **exit 0, 0 real windows, both arms**; A1 exact 82-prefix
on the draw arm; both capture oracles clean. A1's position-71 window permutes 4 of 10
brake-on against 1 of 10 brake-off (Fisher p ~= 0.30, no cost in depth or in A5) —
reported because it is the one number that is not flat.

**PHASE C PART 7 (2026-08-06, session 19): there is no 300x amplifier — and the draw
arm's stall is ONE event, at the first tiled frame.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 7".

Part 6's open question was "what does one delivery do that makes the next one happen,
to the tune of three hundred?" Nothing does. `cpu/chain_stats.h` counts the hand-off
link by link on every run (`ring: chain ...` in `CZ_RING_TRACE`), and on the PM4
control arm every ratio is one or a constant: **`ints/arms` = 0.9997** — the command
processor executes each arm block exactly once — **`isr/ints` = 1.000**, `kicks/isr` =
0.523, and `walks == kicks == drains` to the unit over 173 s. The draw arm's `arms`
column **freezes at 227 seven seconds into the boot** while the numerator keeps
counting, so the "~300x" reads 1.8x at 8 s, 10x at 35 s and 30x at 78 s on one binary.
A frozen denominator was the finding (gotcha 161); part 6's own table already showed it
(437 and 230 armings against the control arm's 14,794) without it being read.

What the freeze IS, traced end to end and reproduced:

- **A single event, not a decay.** For the whole healthy era the D3D worker is never
  used at all — `kicks=0`, `queued=0` at all 986 segment submits. Within one tick the
  worker engages and the guest stops arming for good. The control arm has the identical
  transition at the identical era and survives it.
- **The era is the first TILED frame**, not a file: Resolve's multi-tile path, taken
  once the title starts rendering its scene as two 640-wide tiles (gotcha 118). It is
  the only site in the image that arms `sub_8284AAD0` (82838A94), closes-and-kicks
  (82838AA8) and queues that segment to the worker (82838AD0) in one breath. `#60
  models\zombies.big` is a coincidence of timing (gotcha 164).
- **The engine blocks in the per-frame GPU sync** (`sub_82845230` -> `sub_82845160`),
  `target=1039 emitted=1043 completed=1019`, and never returns.
- **The fence completion word is on a nine-value CAROUSEL, not lagging.** 26,017 GPU
  stores in 100 s; the last 4,000 are 440 laps of `2DF 2E1 2E3 2E5 2E7 2E9 2EB 2ED 2EF`.
  It also visibly REGRESSES between two consecutive sync-wait prints (1023 -> 1017), so
  a wait past the top of the carousel is unsatisfiable rather than slow (gotcha 163).
- **What is being replayed is the arm block itself.** The three most-resubmitted ring
  entries on the draw arm are 93-dword segments — the size of the segment the multi-tile
  Resolve closes around its own arm — at 132/126/86 resubmissions against the control
  arm's worst case of **11**.

**And the obvious cure was RUN and is a negative result.** `CZ_PM4_FENCE_MONOTONIC=1`
refuses any GPU store that moves the completion word backwards; it engages **5,711
times in 90 s** and the boot freezes identically (`arms` pinned at 190, `distinct=2`,
`#60`). So the regression is not what blocks the wait — the wait is for a fence beyond
the top of the carousel, and the segments carrying those `EVENT_WRITE`s are **never
executed at all**, because the ring is saturated with the replayed arm segment. The
missing execution is the fault; the regressing word was its most visible symptom.

Two more hypotheses were drafted and killed by running the same probe on the control arm,
recorded because each looked decisive: the `[obj+0x48]` resume pointer is non-null at
half the drains on **both** arms (1,732:1,731 vs 3,576:3,575), and "6 increments against
1,873" is 1.0 per frame against 3.0 per tiled frame — the same frozen-denominator trap
as the 300x, one screen further down. `CZ_PM4_NO_CP_INTERRUPT=1` is also recorded as a
NEGATIVE result: it cannot isolate the replay, because the boot deadlocks at `boot.bct`
(file #5) without source 1.

Gates unchanged: `--smoke` OK; the control arm reaches `#83 cinezombie.big`;
`truncated=0`, `max` hold streak 1 (control) / 2 (draw).

**PHASE C PART 9 (2026-08-06/07, session 21): the title screen is TWO screens, and four
defects sat between the 3D one and the display.** Details in `docs/phase5-notes.md`
§§6s-6u and `docs/d3d-translation-plan.md` §"Phase C part 9"; hand-off in
`docs/d3d-phase-c10-kickoff.md`.

Part 8 handed over "the 3D background and the DEAD RISING 2 wordmark are black on BOTH
arms" as a phase-5 renderer gap. Measuring all 32 dumped frames of a boot rather than
looking at one says why the claim was half wrong: **49 frames in ~1,000 carry the
DEAD RISING 2 CASE ZERO logo and are a near-exact match for capture E2.** The renderer
had been producing a correct title screen for one frame in twenty, unseen, and it was
the OTHER era — capture **E3**'s animated Still Creek background — that rendered black.
Four defects, each hiding the next, all found and fixed on the **PM4 control arm**:

1. **A stale texture-cache entry composed the whole scene away.** `UploadTexture`
   consulted the fetch-constant cache before the resolve-snapshot check, so the rule the
   code already stated (a snapshot must not be cached) only held for a surface whose
   FIRST fetch already had one. The colour-grading LUT is resolved late in a frame and
   sampled early in the next, so its first fetch during the boot uploaded whatever the
   allocator had left there and froze it for the process. The tone map ends in two LUT
   lookups: a black LUT is a black frame. Tone map output 0.00% -> **95.3%** non-black,
   presented frame 2.31% -> **99.4%**.
2. **The exploded geometry was DRAW_INDX read one dword off** — the index swizzle is the
   TOP two bits of the SIZE dword, and reading it off the ADDRESS also masked away
   address bit 1, real for a 2-byte-aligned 16-bit index buffer (~40% of draws, read one
   index early). Every draw in this title is `8-in-16`. Fixed: a recognisable Still Creek.
3. **A rectangle list's fourth corner was never drawn** (`v0 + v2 - v1`), so half of
   every per-pass CLEAR was missing — and 233,155 draws a boot clear DEPTH ONLY, so the
   previous pass's depth survived in the other half and rejected the scene behind it.
4. **Window coordinates are in PIXELS and our EDRAM is at SAMPLE resolution**, so on the
   4x-MSAA surface the scene tile's clear covers 320 of its 640 columns.

**The title screen's LEFT HALF now renders as a complete, bright Still Creek** — sky,
power lines, the gas station, zombies, the road, the grass.

Every one of the four has a same-binary control arm (`CZ_VK_TEX_CACHE_FIRST`,
`CZ_PM4_INDEX_ADDR_SWIZZLE`, `CZ_VK_RECT_HALF`, `CZ_VK_NO_MSAA_WINDOW_SCALE`), and the
instrument that made 3 and 4 diagnosable is `CZ_VK_NO_DEPTH_TEST=1` (gotcha 173).

**Gates, PM4 arm:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 0 real windows**;
`truncated=0`; deepest file **#83 `cinezombie.big`**; frames presented unchanged.

**PHASE C PART 11 (2026-08-07, session 23): THE RIGHT TILE IS FIXED, AND IT WAS A
PACKET WE NEVER ANSWERED.** `docs/phase5-notes.md` §6x; hand-off in
`docs/d3d-phase-c12-kickoff.md`. The open item below and part 10's whole explanation of
it are superseded — read §6x before either.

`EVENT_WRITE_EXT` with event `0x1A` is the Xenos **screen extent query**: the GPU writes
the rectangle it just rasterized into guest memory, and this title feeds it straight
into the next frame's bin masks. Our command processor decoded that packet and did
nothing with it — 818,507 no-ops a boot — because the fence family's handler stores only
when a packet carries a value dword and this form carries an address and none. So the
guest's own fix-up pass (`sub_8284A7F8`) intersected **uninitialised memory** against
its tile rects and wrote "touches no tile" onto 76% of records, and the right tile's
pass discarded them.

Answering it conservatively — an extent larger than any tile, "this draw may have
touched anything" — with `CZ_PM4_NO_SCREEN_EXTENT=1` as the same-binary control arm:

| | control | fixed | B1 (hardware) |
|---|---|---|---|
| draw packets discarded by the bin rule | 32.7% | **0.28%** | **0.3%** |
| fix-up pass output | 76% `80000000` | 100% `8000000F` | — |
| scene surface median coverage | 56.1 / 53.8% | **99.5 / 99.5%** | — |
| draws per frame (median) | 1,630 / 1,634 | **2,484 / 2,474** | — |

First time this port's predication has agreed with the capture. Two runs per arm,
alternated; 45.7 pp cross-arm against a 1.5 pp band and 0.00 pp within-arm.

**And part 10's three claims about this are RETRACTED in place** (`phase5-notes.md` §6w):
the fix-up pass is not gated shut (3,496 of 3,497 dispatcher entries have it open) and
does not patch zero records (1,751 calls, 388,451 records) — those numbers were a probe
printing only its FIRST call (gotcha 186); `[obj+0x164]` is the current bin SELECT, not
a flags word, and its bit 31 means "tile 0", which is also why the LEFT tile keeps every
draw regardless of mask; and `0x80000000` is not the placeholder but a trailing reset,
the placeholder being the LEADING `SET_BIN_MASK_LO FFFFFFFF`.

**THE OPEN ITEM AS PART 10 LEFT IT — superseded by the above, kept for the trail:** the RIGHT tile
(screen 640..1280) is nearly empty, and it is **ME bin predication**. `gpu/pm4.cpp` has
implemented `(header & 1) && (binMask & binSelect) == 0` since phase 1 and had never
counted it: **a third of this title's draw packets are discarded by it** (1,039,423 of
3,113,236 over 1,579 frames). One frame's two scene passes execute **931** draws and
**23**. `CZ_PM4_BIN_TRACE` prints the pair hardware compares, and the shape is stark —
the tiles select bins `{0,1,31}` and `{2,3}`, and in the `{2,3}` tile 74,773 draws
carrying mask `80000003` and 25,770 carrying `80000000` can never overlap it. A title
does not emit 100,000 unreachable draws a boot, so either the bins are not the
left/right split we assume or the comparison is wrong in one of three places (the 64-bit
LO/HI assembly, the meaning of bit 31, or the ORDER — a mask read one draw late gives
exactly this shape). **The check to run first needs no emulator:** B1 carries the same
`SET_BIN_MASK`/`SET_BIN_SELECT`/`DRAW_INDX` stream, so replaying the rule over it says
whether 8% survival is what hardware does.

**And a number withdrawn before it did damage:** "hardware issues ~2,540 draws a frame
and we issue ~1,620" compares draw PACKETS in a capture against draws the RENDERER
ACCEPTED. At one instant of one run the command processor parses 1,971 packets a frame
and hands the renderer 1,313; the predication eats the difference. `ring: ... draws=N
(predicated out=M)` is now always on, because a mechanism with no counter cannot be
subtracted from a comparison (gotcha 162).

**PHASE C PART 12 (2026-08-07, session 24): the menu panels, localised — and the dead
ISR code deleted.** `docs/phase5-notes.md` §6aa; hand-off in
`docs/d3d-phase-c13-kickoff.md`.

Part 11 handed over "three black panels and malformed label text on the new-game
screen", newly reachable headless via `CZ_FAKE_PRESS_SEQ=START,A,A`. Both are now
localised to a NAMED OBJECT by arms rather than by reading, and the shape part 11
predicted (§6s's — a pass reading a surface the renderer never wrote) is **refuted**:
the panel is the frame's last pass, a 115-draw compose into the front buffer, and its
inputs are all present.

* **The three black rectangles are ONE texture** — `0364B000`, a 16x16 DXT1 whose every
  texel reads zero. `CZ_VK_SKIP_TEX=0364B000` removes exactly those three rectangles and
  **reveals three correct thumbnails underneath**, so everything behind them is right.
  The draws blend `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` (honoured), and an all-zero DXT1 is
  opaque black under BC1 — so hardware's bytes differ from ours, and the open question is
  who writes them. That is a CPU write, so `CZ_PM4_MEM_WATCH` cannot see it; the tool is
  gotcha 143's hardware watchpoint.
* **The malformed text is one of two glyph atlases** — `007C6000` (376x376) garbles,
  `007BB000` (184x184) is perfect, through the SAME `(vs, ps)` pair with every other
  fetch-constant field identical. Two arms cleared the atlas itself: `CZ_VK_TEX_REFRESH`
  (2,250 in-place re-uploads, picture identical) and `CZ_VK_TEX_DUMP` (a clean, correctly
  untiled page of glyphs). That leaves the draw's texture COORDINATES; 376/184 = 2.04 and
  the glyphs read as magnified fragments.

**Part 11's item 4 is closed**: the private walker's `case 0x54:` ISR replication and
`MirrorIsPoisoned()` are deleted, after re-confirming both zeros on a correctly
configured draw arm at `#83` (`arms=12627 ints=12626 isr=12626`, `kicks == walks ==
drains = 6752`, `distinct=885`, `truncated=0`). They guarded a race the brake closed in
parts 4-6, and the counter read zero even on part 6's brake-OFF arm.

Six new instruments, all off by default, and one of them existed only in this file:
`CZ_VK_SKIP_TEX`/`CZ_VK_ONLY_TEX`, `CZ_VK_TEX_CENSUS`, `CZ_VK_TEX_REFRESH`,
`CZ_VK_TEX_DUMP`, `CZ_VK_SNAP_FRAME`, `CZ_VK_FRAME_DUMP_EVERY` — plus
**`CZ_VK_PASS_DRAWS`, which has been documented here since part 9 and was never
implemented** (the count was a hardcoded 4).

**Gates, both arms:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 2 windows,
0 real**; `truncated=0`; deepest file **#83 `cinezombie.big`**.

**PHASE C PART 13 (2026-08-07, session 25): the UI's whole text layer was ONE run
repeated, and the crash 53 files deep was the title's own ASSERTION.**
`docs/phase5-notes.md` §§6ab-6ad and `docs/phase3-notes.md` **finding 50**; hand-off in
`docs/d3d-phase-c14-kickoff.md`.

Part 13's list was the two menu defects then the picture; the item listed LAST as a
frontier turned out to be the biggest thing in the session.

- **The malformed menu text was not the texture coordinates — it was
  `VGT_INDX_OFFSET`.** A draw packet has no base-vertex field, so that register is the
  only way a title sub-allocates ONE dynamic vertex buffer between draws, and this
  title's entire UI works that way: 115 draws whose fetch constant never changes
  address, with the offset advancing by exactly the previous draw's index count. The
  renderer printed the register in `CZ_VK_STATE_PROBE` and applied it in none of its
  three submission paths, so **every draw rendered the FIRST run's vertices** — one text
  run correct, every other one that same run's glyphs through whichever atlas it bound.
  Part 12's attribution to the two glyph ATLASES is retracted: that is the visible
  difference between the draws and the cause of neither. The save-slot screen now
  renders `SLOT 1/2/3`, `- NEW GAME -`, `GAMER PROFILE`, `Player`, `LV. N/A`, the
  PP/Money rows and the `A SELECT / B BACK / Y DEVICE SELECTOR` legend.
  `CZ_VK_NO_INDX_OFFSET=1` is the control arm; it engages 211 times in a plain
  title-screen boot and on essentially every menu draw, which is why five phases of
  scene work never saw it.
- **The black panels: nothing writes them.** Three hardware watchpoints, one per
  physical alias, through the whole menu era: **zero hits**, and no resolve targets that
  address. Part 12's inference that "hardware's bytes differ from ours" is retracted for
  a measurement — the title binds a 16x16 DXT1 it never fills, on three draws for three
  EMPTY save slots. ~~The one remaining test is a run with a real save present.~~
  **CLOSED in part 14 by that test: the panel is the save's THUMBNAIL, and black is the
  correct picture for a slot with no valid content** (`docs/phase3-notes.md` finding 51).
- **The SIGSEGV at file #137 was `dbAssert(0 && "Bad file digest.  Please re-link the
  executable and try again.")` from `digestmanager.cpp`** — not a memory bug. It looks
  like one because XenonRecomp lowers `twi` to nothing, so the deliberate `stw r26,0(0)`
  that follows the trap is what faults. Three links, each measured, none of them
  changing an observable alone: `XexGetModuleSection` answered nothing ever (its comment
  was written about a runtime with no loader and never re-asked); the XEX RESOURCES it
  should answer from live in `.idata`, which `main.cpp` skipped by NAME under a comment
  describing a bounds condition only `.reloc` meets; and the SHA-1 the digest manager
  calls is three kernel imports left as generated stubs — and a stub is the wrong shape
  for a hash, because no digest value means "not implemented", so the guest compared
  twenty zero bytes and refused to run.
  **Result: #137 + SIGSEGV -> #154 `skeleton\childfullbody.big`, zero faults over
  300 s** — 71 files past anything this project had reached.
- **The picture against capture E, asked cleanly for the first time.** No transform
  (every frame's best orientation is `identity`, runner-up 0.14-0.35 behind), and four
  named differences: the whole frame is uniformly out of focus AT EVERY DEPTH where E3
  is sharp except in the far distance; colour is flat and green-shifted; the
  `(C) CAPCOM CO., LTD. 2010` line and one sign's lettering are missing; the `GAS`
  balloon and the street bunting are blank.

**Gates, BOTH arms:** `--smoke` OK; A5 **exit 0, 0 real windows**; `truncated=0`;
deepest file on a no-input boot **#83 `cinezombie.big`**. A1 is an exact 84-prefix on
the PM4 arm and hit the long-known position-71 scheduling window on the draw arm
(gotcha 86). The draw arm's chain is the healthy shape — `arms=12741 ints=12740
isr=12740`, `kicks == walks == drains = 6776`, `distinct=813` — and it applies the base
vertex 2,258 times a boot, because `d3d_draw.cpp`'s `SetReg` is generic and the register
lands in its private file by construction.

**PHASE C PART 14 (2026-08-07, session 26): a resolve has a SOURCE, and it was the
blur — three of the four picture defects at once.** `docs/phase5-notes.md` §6ae and
`docs/d3d-translation-plan.md` §"Phase C part 14".

Part 14's list was the frontier at `#154`, then the blur, then the other three picture
differences. The first item dissolved on measurement and the second turned out to
contain the third and fourth.

- **`#154` was never a frontier, and the boot is not stalled.** `NtCreateFile`
  successes stop climbing because the title stops OPENING files, not because it stops
  loading: with `CZ_FILE_TRACE=1` the reads run on for another ~40 s out of `.big`
  containers it already has open — `npcs.big`, `cine_props.big`, `streamedassets.big` —
  through the prologue cinematic's props, ending with three `XMACreateContext` calls.
  Throughout, the ring chain is the healthy shape, `truncated=0`, frames keep presenting
  at ~1,200 draws each, and every `[wait]` is an idle worker or one of the two threads
  the title blocks by design (finding 41). Gotcha 206.
- **The blur was `RB_COPY_CONTROL`'s `copy_src_select`** — three bits this renderer read
  nowhere. **18.4% of this title's resolves copy the DEPTH buffer** (10,448 of 56,925 in
  B1, `tools/xtr_resolve_census.py`): three shadow cascades and the scene depth. The
  depth-of-field pass was therefore computing its circle of confusion out of the scene's
  own COLOUR, saturating it, and compositing full blur over every pixel at every depth.
  Two runs per arm, alternated, `CZ_VK_NO_DEPTH_RESOLVE=1` as the control: median
  mean-|gradient| **1.185/1.204 -> 7.640/7.666** (6.47x, no overlap), median distinct
  colours on the scene colour surface 72,740/72,711 -> **85,555/85,752**, frames per
  85 s 859/848 -> 803/811. **It closes §6ad's items 1, 3 and 4 together** — the missing
  `POP 753` and community-watch sign lettering and the absent bunting and gas-station
  signage were fine detail the blur erased — and moves item 2 (colour) a long way.
- **No aggregate over pixel VALUES could see it.** Coverage moved **0.01 pp**, inside
  `frame_compare.py`'s own 1.5 pp band, so this project's purpose-built renderer A/B
  metric reported "no detectable difference" about its largest visible defect. Gotcha
  135 in a second disguise, and `tools/frame_sharpness.py` is the instrument for it.
- **RETRACTION: `06BE4000` is the scene DEPTH.** It has been documented here as "the
  scene" since phase 5 and used as `CZ_VK_FRAME_STATS_SURFACE` for every renderer A/B
  in this port — and it held colour pixels only BECAUSE of the defect above. The scene
  colour is **`0684B000`** (`0685F000` for the second tile); the depth's tiles are
  `06BE4000`/`06BF8000`. Earlier measurements stand; the label did not (gotcha 205).
- **`CZ_VK_RESOLVE_TRACE_PASSES` did not exist**, for the second time in three sessions
  (gotcha 193) — and the budget it names still counted 60 HEADER lines while the two
  follow-up lines printed uncapped, which is the exact defect part 9's note says it
  fixed. Now real.

**Gates, PM4 arm, renderer on:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0,
2 windows, 0 real**; `truncated=0`; deepest file on a no-input boot **#83
`cinezombie.big`**.

**PHASE C PART 15 (2026-08-07, session 27): the prologue's black screen was THREE
defects and only two were ours — the renderer draws the prologue.**
`docs/phase5-notes.md` §§6af-6ag; hand-off in `docs/d3d-phase-c16-kickoff.md`.

Part 14 handed over "a live pass with live inputs produces black, the same SHAPE as
§6s". It is a stack, and the bottom of it belongs to the guest:

1. **A shader the cache did not have.** `vs_24e60d91249e6d04`, 351 dwords, loaded by
   the prologue and in NEITHER capture (A1 stops at the title screen, A2 is gameplay)
   nor in our own dump, whose recipe built from a plain boot that also stops at the
   title screen. **28,718 draws a run declined**, reported as one log line and a
   counter. Fixed (337 shaders now, and the recipe reaches the prologue) — **and it did
   not change the picture.** A real defect hiding behind a bigger one.
2. **The colour-grading LUT's resolve snapshot EXPIRED.** The rule was "taken this
   frame or last", which is right for a post pass reading an earlier pass of the same
   frame and wrong for a surface the title resolves ONCE. The title screen re-renders
   all three LUTs every frame so the window never bound; the prologue's grade is
   static, so the fetch fell through to guest memory, which is zero. Measured with (3)
   patched out: **0.00% -> 99.99% non-black, 1 -> ~89,450 colours**.
   `CZ_VK_SNAPSHOT_MAX_AGE=1` is the control arm.
3. **The rest is the GUEST asking for black, and the compose is faithful.** Printing
   the tone map's constants rather than reasoning about them: `pc(111).x`, the
   vignette POWER, is **0** at the prologue against 1.0 at the title screen, and
   `pc(110).w`, its strength, is **1.0** against 0.4. `pow(x,0) == 1` at every pixel,
   so the compose lerps 100% to `pc(110).xyz` = black. The LUT arithmetic constants are
   bit-identical between the eras. Proved with an arm, not argued: one line patched in
   `ps_114c4965eaabd54c` under `CZ_SHADER_SPV` takes the frame to **99.99% non-black,
   ~89,450 colours**, and the picture is the prologue's opening highway into Still
   Creek, tone-mapped and graded.

**What is actually wrong is upstream of the renderer.** `CZ_VK_FRAME_STATS` over the
black era: the **camera fingerprint is ONE constant value for 1,700+ frames** and the
scene surface's mean luminance is pinned at 104.484, while the draw stream still moves
(1,225-1,247 draws, 848k-883k vertices). The ring chain is the healthy shape
(`arms=11489 ints=11483 isr=11483`, `kicks == walks == drains`, `distinct=764`,
`truncated=0`) and every `[wait]` is an idle worker. The prologue is **stuck in a
faded-out state**. Leading hypothesis, stated as one: the cinematic is cued off audio,
the pump submits **55,808 driver frames of peak exactly 0.0000** because there is no XMA
decoder, and it waits forever.

**And one shadow-cascade defect of ours, with the title's own numbers beside it.** A
window-coordinate draw was mapped through the PRESENTED FRAME's 1280x720 rather than
the EDRAM's 1280x1024. The arithmetic is an identity either way so nothing looked
wrong; the CLIP is not, so every such draw taller than the screen was cut at row 719.
Cascade non-black **12.82% -> 13.28%**, which is the clipped 64x304 strip to the pixel
(`CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1` is the control arm). The rest of the empty half
is the title's: `CZ_VK_DRAW_PROBE` says its clear rects for a 1024x1024 cascade are
**`(0,0)-(480,512)` and `(960,0)-(1024,1024)`**, at z=1.0 with compare func ALWAYS.
**Shadows still do not appear** — committed on mechanism plus a matching structural
delta, which part 14's own rule says to declare.

**Gates, PM4 arm, renderer on:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0,
2 windows, 0 real**; `truncated=0`, 0 parser stalls; deepest file on a no-input boot
**#83 `cinezombie.big`**; presented frame 98.99% non-black at the title screen.

**PHASE C PART 16 (2026-08-07, session 28): four wrong answers removed from the
prologue, and part 15's own conclusion confirmed.** `docs/phase5-notes.md` §6ah;
hand-off in `docs/d3d-phase-c17-kickoff.md`. This session is mostly **negative
results** — each cost a build and a run, each has a same-binary arm behind it, and
that is what stops the next session paying for them again.

First, the timeline nobody had written down. Collapsing `CZ_VK_FRAME_STATS` on the
camera fingerprint turns "the run freezes" into four eras: the title screen (frames
1..591, 2,514 draws, a new camera every frame), the **loading screen** (596..962, ~150
draws, ~36% coverage, 4-5 cameras cycling), the world's first frame (974), and then
frozen from 1002 (1,225-1,247 draws, ~849,000 vertices, presented frame 0.00%). The
loading COMPLETED; the scene surface's mean luminance is pinned at **104.484 to three
decimals**, so the world is not merely hidden, it is not being simulated. The last
files opened name what the title was about to do: `#146 cinematics\cinematics.big`,
`#147 anim\cinematic\701_chuck_arrives_in_town.big`, `#148 skeleton\cineplayer.big`.
**It is sitting at the start of the first cinematic.**

- **NOT AUDIO — refuted, not merely unconfirmed.** Part 15's evidence was a peak
  amplitude of 0.0000, which is a fact about our OUTPUT that no guest code can see.
  The image states the real mechanism: `sub_8285EFE0` reads the XMA context's two
  input-buffer-VALID bits, `sub_82862A90` ORs them into IsPlaying, `sub_82864808`
  caches the answer at `voice+0x120` and branches on the transition. The guest sets
  those bits; the DECODER clears them — so with no decoder every voice ever started is
  still playing (measured: 284,373 polls, 284,354 "playing", **0 stop edges**).
  `CZ_XMA_NULL_DECODER` supplies the missing half, and **all three configurations of
  one binary give the identical frozen frame**: always-playing (stock), never-playing
  (instant consume, `playing=0/318,631`), and plays-then-ends (40 ms/packet, 19 start
  / 18 stop edges). Both polarities and the transition between them.
- **NOT A DEADLOCK.** `gdb -p` over all 31 threads, joined to guest tids by the
  always-on thread trace: exactly ONE thread is in guest code and it is the Draw Thread
  doing its ordinary per-frame GPU sync. The MAIN guest thread is in an infinite
  `NtWaitForSingleObjectEx` that `CZ_WAIT_TRACE` never reports — i.e. it is being
  signalled and re-entered, so the main loop is turning. `[kcall]`'s first-occurrence
  list ends at `XeCryptShaFinal`: **the prologue era reaches no new kernel import**, so
  the blocker is not a stub we have yet to write.
- **NOT OUR SYNTHETIC INPUT.** `CZ_FAKE_PRESS_SEQ` holds its last button forever, so
  every prologue observation this port ever made was taken while the title was being
  poked with A every 8 s. `NONE` now exists. Ten A presses then NONE — no input for the
  last ~170 s — reaches `#154` and the identical state.
- **PART 15 WAS RIGHT, and it was worth re-asking** (gotcha 172): a constant that is
  WRONG and one the guest never wrote look identical from inside a shader. Over the
  black era the guest writes `pc(110) = (0,0,0,1.0)` and `pc(111) = (0,0,0,0)`, **5,662
  times each with exactly one distinct value per register**. The full-black fade is the
  guest's and the renderer draws it faithfully.
- **The engine has its OWN log, and it is switched off.** `sub_827877C8` is a vsnprintf
  with **640 distinct callers** feeding one sink; `CZ_GUEST_LOG=1` hooks it. It prints
  nothing today and the zero is checked rather than believed — the call sites are each
  gated on a debug byte a shipped build leaves at zero. Raising them is the highest
  leverage item on the board (gotcha 215). `game:\cl.txt` is **not** the switch: it is
  read as a CHANGELIST NUMBER.
- **One real defect, recorded rather than fixed.** `VfsTranslate` returns empty for any
  path with no `:`, so a guest path with no device prefix can never resolve. A boot
  makes 29 such opens (`data\anim\weapon\<Weapon>.big`); none of those files exist under
  any prefix, so nothing is currently lost.

**Gates, PM4 arm, renderer on:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 2
windows, 0 real**; both capture oracles clean; `truncated=0`, 0 parser stalls,
`max=2`; `no translated shader` = 0; deepest file on a no-input boot **#83**.

**OPERATOR SESSION ON THE PART-16 BINARY: GAMEPLAY IS REACHABLE, AND THE PROLOGUE
FREEZE IS "CINEMATICS NEVER END".** `docs/phase5-notes.md` §6ai. The most informative
hour this port has had, and no instrument produced it. The operator **skipped both
prologue cinematics and played** — Zombrex tutorial card, watch/MESSAGES screen, Still
Creek, combo weapons. A combo-weapon cutscene then parked the camera on the workbench
with no HUD while Chuck still took input, and **skipping that cutscene restored the
camera**. So:

> **Every cinematic in this title starts and never ends. Skipping is the only exit,
> and the skip path works perfectly.**

The prologue's black screen is that defect wearing §6af's fade. Part 16's four negative
results all stand and are now EXPLAINED rather than merely true — a cinematic that never
advances asks nothing of the kernel, blocks no thread, and does not care whether audio
finishes. **The skip path being clean is the strongest clue on the board**: it runs the
same teardown a natural end would run and it demonstrably restores camera, HUD and
control, so the teardown is fine and only the TRIGGER is missing. A alone does not skip.

And the first like-for-like GAMEPLAY comparison this port has been able to make. **The
HUD is NOT a defect** — an indoor frame missing most of it was written up as one and
retracted within the hour when it appeared in full outside; the safehouse just has not
raised it yet, and capture E4 is a LATER first-gameplay frame than the one it was being
compared against (gotcha 127, applied to a whole screen rather than a metric).

**The colour is**, and the exterior names it far better than the interior:

| | hardware | ours |
|---|---|---|
| safehouse interior (vs E4) | warm red/brown wood, bright orange shirt | green-shifted, blacks crushed |
| Still Creek exterior, daylight | pale hazy blue sky | **the sky is PINK/MAGENTA** |

Chuck's orange shirt, the red car and the yellow LIFE pips are all CORRECT in that same
frame, so this is not a tint or an exposure error — those would move the shirt too. A hue
error that spares saturated reds and yellows while turning a pale blue sky magenta and
the mid greys green is the signature of a **wrong colour-grading LUT**, which is exactly
what §6s proved this frame depends on completely and §6af caught silently expiring. It is
item 6 below, at last visible somewhere it cannot hide.

**PHASE C PART 17 (2026-08-08, session 29): the world is reachable headless, the frame
is PROFILED, and the renderer is not where it goes.** Hand-off in
`docs/d3d-phase-c18-kickoff.md`.

* **Analog sticks in the synthetic-input arm.** `LSUP/LSDOWN/LSLEFT/LSRIGHT` walk Chuck,
  `RSUP/RSDOWN/RSLEFT/RSRIGHT` aim the camera; a stick entry HOLDS for its interval where
  a button entry taps for 150 ms. Verified: `LSUP` walks him from mid-safehouse to the
  door and raises its "Open (B)" prompt, `RSRIGHT` swings the camera ~90°. This closes
  the other half of gotcha 190 — menus were reachable headless and the WORLD was not, so
  every gameplay defect on the board was an operator report with no reproduction.
* **THE FRAME RATE IS NOT THE RENDERER'S, and this is measured rather than argued.**
  `CZ_VK_PROFILE` on gameplay: **the entire renderer is 8.6% of an 87 ms frame**, against
  33% GPU wait and 58% guest code + command processor, at 134% process CPU. Making the
  renderer instant buys ~1.09x. **My own prime suspect before measuring — the per-draw
  constant upload, 8 KB x ~1,900 draws = ~19 MB a frame — is 0.5%** (gotcha 80: write the
  oracle before the theory, and this session did it backwards).
* **One real win: the readback buffer was WRITE-COMBINED.** Presenting a frame read
  3.7 MB back uncached at ~230 MB/s. `readback` 15.7% -> 0.4%, frame 103 -> 87 ms, 9.7 ->
  11.5 fps headless and ~10 -> ~15 windowed. `CZ_VK_READBACK_UNCACHED=1` is the control.
* **`CZ_VK_SNAP_ON_BLACK`** captures the view-dependent black by itself. The operator
  produced its first BEFORE/AFTER pair, and the DIRECTION names the mechanism: a
  degenerate auto-exposure that saturated would go WHITE, and this goes BLACK — which is
  what `exposure = key / averageLuminance` does when the average is enormous or
  non-finite, with an enormous blown-out glow sitting in the "before" frame.
* **Two retractions, both from ordinary play** (the part-17 pattern, again): the Still
  Creek **pause menu is pixel-correct**, and **Dick renders**. Re-check Fausto's legs and
  Gemini's hair before working item 3d.
* **The save's failure MOVED rather than persisting.** `XUserWriteAchievements` no longer
  returns E_FAIL and `XamContentCreateEx` mounts. Its `XamContentClose` is ~10 lines
  later with no file activity between — **which is NOT evidence that nothing was
  written**, because `NtCreateFile` successes print only for the first 512 and then every
  64th and that run was thousands of opens deep (gotcha 109). One save with
  `CZ_FILE_TRACE=1 CZ_SAVE_PROBE=1` settles it.
* **Operational, and it cost the session's best evidence: `/tmp` is a TMPFS on this
  machine**, 32 GB, and it was sitting at 24 GB of scratch from earlier sessions. A full
  one makes `cz_runtime` runs fail with `write error: Disk quota exceeded`, which
  presents as the SHELL DYING seconds after a run, and it stopped the operator saving
  screenshots — losing both frames of the black-transition pair. `df -h /tmp` before a
  run, not after a mystery. Note the misdiagnosis too: this session's own ~1 GB of frame
  dumps were blamed for the whole thing, and deleting them changed the total by 4%.

**PHASE C PART 18 (2026-08-08, session 30): the frame rate was 11.8 fps because the
graphics pump was ASLEEP for 57% of every frame — it is ~29 fps now, and no work was
deleted.** `docs/phase5-notes.md` §§6aj-6am; the plan executed is
`docs/perf-plan-overnight.md`.

Its §1 said to attribute the 58% that `CZ_VK_PROFILE` calls `outside` before optimising
anything, on the grounds that it contains four different investigations. It contains
one, and it is not work.

* **`perf` says the CPU is a guest busy-wait, and that is a red herring.** 38.1% of all
  cycles in `sub_8283C6C8` and 21.3% in `sub_82845160` — finding 38's ring-progress
  spin, 73% of the process with the ladders, 77.6% in a single thread — while the thread
  running the command processor AND the whole renderer uses 0.14 of a core. A cycles
  profile cannot see the thread that decides when a frame ends, because it is asleep
  (gotcha 218).
* **`gpu/pump_stats.h` can: 3.00 pump ticks per frame, every window, and 57% of the wall
  clock in `sleep_for(16 ms)`** — 48 ms of an 85 ms frame. The walk stops at every
  unsatisfied WAIT_REG_MEM and resumes on the next tick, so each hand-off wait cost a
  whole sleep period.
* **`CZ_PM4_TICK_MS` splits the ring tick from the vblank: 84.4 -> 69.9 ms (1.21x)**,
  with `submit` identical at 24.0 ms on both arms, i.e. the entire delta is sleep.
* **`CZ_VBLANK_TICKCOUNT` names a defect older than phase C: the vblank has never been
  60 Hz.** It was delivered every N loop ITERATIONS, and an iteration is a sleep plus a
  ring walk — so the GPU's wait pushed the guest's vblank out. 40.2/s on the old loop,
  31.2/s once the ring ticked faster, **62.2/s on a deadline**, and 2.0x, because the
  CP's per-frame waits are released by the swap-queue walker inside that very ISR.
* **The GPU has been at 210 MHz of 2100 for every measurement this port has ever made**
  (gotcha 219). `submit` split into `[call 0.1% gpu 35.4%]`, so there is no host-side
  driver cost — and `nvidia-smi` says P8, 15.7 W of 240 W, reason "Idle: Active".
  **This retires the overnight plan's §2a and §2c until one root command answers it.**

**AND THEN THE OPERATOR PLAYED IT, which moved the whole problem.** A Still Creek
zombie crowd renders **4,800-6,800 draws a frame** against the headless recipe's 1,930 —
3.5x the workload every conclusion in this port had been based on (gotcha 222). Three
results:

* **The GPU clock question is ANSWERED.** `sudo nvidia-smi -pm 1` + `-lgc 2100,2100`
  takes the card from P8/210 MHz to 1950, and the GPU term in a crowd frame from
  **18.8 ms to 6.64 ms — 2.9x, now the SMALLEST term**. Crowd fps 15-25 -> 22-25.
  Invisible everywhere else, because everything else is on the title's own cap.
* **Per-draw state binding is cached** (`CZ_VK_NO_STATE_CACHE=1` is the arm): 11.4% off
  `record`, engagement 76-99.9% against a 0.0% negative control. Small — 1.6 pp of an
  ordinary frame, ~3% of a crowd — and recorded as small.
* **A crowd frame is now 75% our own CPU**, in two roughly equal halves: renderer draw
  path 21.4 ms and PM4 walk 11.0 ms, both linear in the draw count, neither ever
  optimised. **`docs/perf-cpu-plan.md` is the plan**, and its item 0 blocks the rest —
  the headless recipe cannot reach a crowd, so every A/B would run against the vblank
  cap where a CPU saving measures as exactly zero.

**Gates:** `--smoke` OK; A5 **exit 0, 0 real windows**; `truncated=0`; `no translated
shader` = 0; deepest file **#83 `cinezombie.big`**; swap queue one record in flight
(head 8547 / tail 8548), not a queue nobody drains; the picture unchanged — a dumped
boot's `frame_003456` matches capture E2 at **+0.959, identity orientation**.
**The cost, stated: A1's position-71 window now permutes on every run** (1 of 10 -> 5 of
10 -> every run, over the two changes). Same six-name two-thread interleave, mechanism
in §6ak, and `CZ_PM4_TICK_MS=16 CZ_VBLANK_TICKCOUNT=1` restores the exact 84-prefix in
one command — but `kernel_call_diff.py` refuses to relax the masked gate for
permutations on purpose, so it is a real loss and not a technicality (gotcha 221).

**PHASE C PART 19 (2026-08-08, session 31): the port's top rendering defect was a
128 MB allocator, the save could not write, and the headless recipe now reaches the
outdoor world.** `docs/phase5-notes.md` §§6ao-6ap, `docs/phase3-notes.md` finding 52.

The part-18 kickoff's list, worked in its own order. Item 1 was already done (part 17
committed and controlled `CZ_VK_SNAP_ON_BLACK`); item 4's performance section was
superseded by the overnight session before this one started.

* **THE VIEW-DEPENDENT WHOLE-FRAME BLACK IS SOLVED, and the auto-exposure hypothesis
  that led the board for six parts is REFUTED.** It is the renderer's per-frame bump
  ARENA overflowing: `ArenaAlloc` skips every draw it cannot satisfy, this title's post
  chain is at the END of the frame, and the arena was a fixed 128 MB against a true peak
  of 161. A frame with enough geometry in it therefore lost its downsamples, its
  luminance ladder, its three colour LUTs and its tone map, and presented black with a
  correctly rendered scene sitting in EDRAM behind it. "View-dependent" was literal:
  which way the camera points decides how much geometry is in the frame.
  **One binary, two arms: 128 MB gives 160 black frames of 8,216 gameplay frames,
  512 MB gives zero — and every one of the 160 is the frame immediately after an
  `arena EXHAUSTED` line, 160 of 160.** The arena grows now rather than being a bigger
  number (`CZ_VK_NO_ARENA_GROWTH=1` is the control); what is NOT closed is the
  consumption, ~27 KB a draw, 8 KB of which is the constant block `perf-cpu-plan.md`
  §1a-D already wants deduplicated for an unrelated reason.
* **The resolve-chain dump is what named it, and the pair is why.** `CZ_VK_SNAP_ON_BLACK`
  fires on COVERAGE and in a gameplay run catches only loading screens, which are
  legitimately black. `CZ_VK_SNAP_ON_DARK` fires on mean luminance and **dumps a BRIGHT
  REFERENCE chain from the same location seconds later** — one dark chain is equally
  consistent with "this pass is broken" and "the scene really is dark here" (gotcha 133
  turned into an instrument). Read side by side, the black frame's scene colour was
  98.4% lit at mean 35.7 and every downstream surface was identically zero. That is what
  ruled the tone map, the exposure and the grade out: they are victims in the same list.
* **A second, independent renderer defect found on the way: a snapshot is PITCH-sized
  and a fetch is WIDTH-sized.** Where those differ every texture coordinate is scaled by
  width/pitch, which is invisible on full-screen surfaces and compounding on a reduction
  ladder — and the 2x1 scene-average luminance the tone map reads was identically ZERO
  in every frame of every era. Predicted lit-column counts from that one ratio match five
  consecutive links exactly, before and after. Fixed with right-sized views (9 created in
  a boot, refreshed free inside the resolve that writes their source), after a counter
  said the mismatch is 3.3% of fetches rather than a general problem needing a general
  mechanism.
* **THE SAVE WORKS — end to end, and cross-checked against the hardware save.** The
  operator played to a save point on this binary and the title reported **"Game saved
  successfully"** with the slot panel filled in and a rendered thumbnail. The log is A3's
  exact sequence (`XamContentCreateEx -> mounted`, `NtCreateFile ... WRITABLE,
  disposition 5 (created)`, `NtWriteFile ... 303104 written`, `XamContentClose`), and the
  file on disk is **303,104 bytes with bytes 4..31 IDENTICAL to A3's real 360 save** and
  the same non-zero region boundaries; only the leading four bytes differ, which is a
  checksum or a timestamp. That RETRACTS open-items 1b: the unhandled XGI message it
  blamed does not appear in the successful run at all — it was fixed by part 16 and the
  item outlived the defect. What is still untested is the LOAD half, and it is now a
  better test than it has ever been, because the save on disk is OURS rather than A3's
  (whose foreign profile GUID was the confound under open-items 2's `Damaged Content`).
* **AND IT LOADS. The save round trip is closed** — the first title state in this port
  that survives a process exit. The first Load attempt failed with **"Load failed. File
  appears to be corrupt."** with the file untouched on disk, and the log named the real
  cause: `XexGetProcedureAddress ord=0x271 -> NOT_FOUND`, and **the file was never
  opened**. The title's own error text named the wrong subsystem, which is what three
  sessions of reading `Damaged Content` as evidence about the save's CONTENTS had been
  worth. **Ordinal `0x271` is `XamContentCreateInternal`**, named from A3's own load-back
  and from the guest's call site rather than from an ordinal table — `sub_825D8F30`
  builds an `XCONTENT_DATA` on its stack with a **42-byte** file-name field, which is
  exactly `XCONTENT_DATA::szFileName`, and passes flags 3 = `OPEN_EXISTING`. It is the
  same mount `XamContentCreateEx` already did, so the two are now one shared body.
  **This CLOSES open-items 2 and retires its per-profile-signature explanation as
  wrong**: A3's save was never ours to load. And `kResolvable` is no longer "the seven
  A1 resolves" — A1 was captured with an EMPTY save root, the one configuration in which
  the load path never runs (gotchas 45 and 106, for the second time).
* **THE SAVE COULD NOT WRITE, IN TWO INDEPENDENT WAYS, and neither was visible.**
  `NtCreateFile` ignored `createDisposition` entirely and opened every handle `"rb"`;
  `NtWriteFile` was a generated honest-failure stub. Either alone produces exactly the
  symptom part 17 recorded, which is why the symptom could not discriminate. Both are
  implemented from A3 (six dispositions, mode from the guest's own access mask,
  `IO_STATUS_BLOCK.Information` per outcome, and the write path's EVENT completion where
  the read path uses an APC). A THIRD defect only a test could find:
  `CZ_FILE_WRITE_SELFTEST` wrote 303,104 bytes and then could not re-open them, because
  `VfsResolveExisting` caches NEGATIVE results and the create's own existence check is
  what caches the "no". **What is still not known is whether the title's save completes**
  — no headless recipe reaches a save point — but every file operation off `game:`/`d:`
  is now logged uncapped, so the printer can no longer be the reason for a silence.
* **`docs/perf-cpu-plan.md` item 0 is CLOSED: the headless recipe reaches the outdoor
  world at 6,400-8,100 draws a frame**, past the operator's 6,592. One extra `B` at the
  safehouse door and alternating `LSUP` with `RSRIGHT`/`RSLEFT`. It is in `CLAUDE.md`
  beside the safehouse recipe, and it is what reproduced the black in the first place —
  the same argument the plan made for why it had to come first, paid off twice.
* `tools/snap_dump_stats.py` summarises a 61-surface snapshot dump as one line per
  surface, because "open them and look" is gotcha 133 applied to a directory.
* One shader recovered from a run that reached new ground; the cache is **371**.

**Gates:** `--smoke` OK; A5 **exit 0, 0 real windows**; `truncated=0`; deepest file
**#83 `cinezombie.big`**; `no translated shader` = 0; the picture matches capture E2 at
**+0.9597, identity**, against the pre-fix arm's +0.959; median scene coverage inside
the 1.5 pp band across the renderer change.

**And a correction to part 18's gate note, paid for by running it twice.** Part 18
recorded that `CZ_PM4_TICK_MS=16 CZ_VBLANK_TICKCOUNT=1` "restores the exact 84-prefix in
one command". It does not, reliably: two runs of that exact configuration this session
gave an exact prefix once and the familiar position-71 rotation
(`XamUserCheckPrivilege` / `XexGetModuleHandle` / `XexGetModuleSection`, three names,
same set) the other time. The flags REDUCE the interleave's frequency; they do not
remove it, and `kernel_call_diff.py` exits 1 on the run where it appears. That makes
every single-run "exact prefix" claim a coin flip (gotcha 159), which is exactly what
gotcha 221 says about this window and what part 18's own wording softened. The
set-based A5 gate is the one that holds, and it holds.


**PHASE C PART 20 (2026-08-08, session 32): the profiler was counting nested phases
twice, and the CPU plan was ranked on the result.**

The session began where `d3d-phase-c19-kickoff.md` points — `docs/perf-cpu-plan.md`,
"now fully runnable and the biggest single item" — and the kickoff's own warning is what
it found: **re-measure before optimising.** Not because the numbers were stale, but
because they were wrong.

* **`ProfScope` accumulated INCLUSIVE time and the scopes nest.** `record` opens partway
  down `DoDraw` and lives to the end of it, so the `UploadStream` calls below it ran
  inside it: their cost landed in `streams` AND in `record`. The frame print then derived
  DoDraw's residual by subtracting the named phases from the whole, removing `streams`
  twice. `record` read 11.07 ms of a 21.40 ms draw path and the residual read 0.91 —
  where the true split is 6.30 and 5.68. **The plan's §1 and §2 are ranked by expected
  size, so the ranking was wrong**, and it filed its second-biggest item as "the cheapest
  item in this document".
  What makes this class hard is that nothing looks wrong: the error MOVES time out of the
  outer scope's residual into an inner scope's name, so every column still sums to the
  total and the `outside` column — which exists to show what the instrument cannot
  account for — is unaffected. Gotcha 228, whose corollary is that a profiler is
  instrumentation and gotcha 30 applies to it: break it on purpose and check the columns
  move as predicted. Neither this port nor the two before it had ever done that.
* **The corrected crowd frame, re-measured at 6,737-6,806 draws** (P8/210 MHz of 2100 —
  no passwordless sudo this session, so the fence column is inflated ~2.9x and nothing
  else is): `record` 6.7 ms, `other` 5.6, `streams` 3.7, `textures` 2.7, `constants` 1.3,
  draw path **19.9 ms (36.5%)**; PM4 walk **11.8 ms (21.6%)**, unaffected by the defect
  because it was always derived from the renderer's INCLUSIVE total. `[vkprof]` now
  prints the walk's own cost as a `pm4` column instead of leaving it to be subtracted by
  hand.
* **The per-draw path was carrying its own instrumentation**, which gotcha 223 had warned
  about in this project the day before: five `Count()` calls (each a `std::string`
  construction and a red-black tree walk), four `getenv` calls for instruments nobody had
  enabled, and one `snprintf` formatting the draw's shader hash for `[psbind]` — **above**
  the gate that tests whether `CZ_VK_PSBIND` is set. That last is its own rule now
  (gotcha 230): `docs/instruments.md` promises every arm is "free when off", and a gate
  around the `fprintf` does not deliver that when the work feeding it sits outside the
  gate. `COUNT(literal)` resolves each counter's ADDRESS once per call site into a
  function-local static; names, order and the stats block are untouched. At matched draw
  counts: **`record` −47%, `other` −49%**, four points of frame time.
* **§1a hypothesis A measured and mostly refuted.** The plan's leading idea for `record`
  was that a crowd rebinds the same vertex buffer over and over, and it prescribes
  counters before code. In the crowd era **34% of vertex binds and 22% of index binds
  repeat the previous offset** — ~1.3 of the ~6.4 `vkCmd*` calls a draw, worth ~1.4 ms of
  a 54 ms frame. Real, about a third of what the hypothesis expected, and below the
  noise floor, so it can only ever be claimed from the counter. Not acted on, as asked.
* **THE NOISE FLOOR, which is the most transferable thing here.** `tools/frame_perf_bins.py`
  compares two `CZ_VK_FRAME_STATS` files BINNED BY DRAW COUNT rather than averaged over a
  run, because the crowd recipe is 57 fixed 8-second steps against a boot whose depth in
  wall time is a distribution — so a whole-run mean is dominated by the capped safehouse
  era. That binning is necessary and **it is not sufficient**: two runs of ONE binary
  disagree by 10-13% in exactly the crowd bins, with the tool's own standard-error column
  reading up to 22 sigma, because consecutive frames in a bin share a camera and a
  location and are nowhere near independent samples. Gotcha 229: run the A/B with nothing
  changed first, and treat what it prints as the floor.


---

**PHASE C PART 21 (2026-08-08, session 33): the stream cache is 94% hits and still
copies 74 MB a frame, and the arena growth was being charged to the draw path**

Two items off the part-20 hand-off's ordered list, both of which it had ranked from the
operator session. Neither is a speculative change: item 1 was explicitly "count before
writing anything", and item 2 was a call-graph observation with a measured spike.

* **`perf-cpu-plan.md` §1b is ANSWERED, and it answers the opposite way to the guess in
  its own text.** The plan set up the ambiguity correctly — nearly all hits means the
  cost is the lookup and the fix is a cheaper key; mostly misses means real copying and
  the fix is a different cache lifetime — and said no reading of the code could decide
  it. `CZ_VK_STREAM_CENSUS=1|2` decided it. At ~6,400 draws in a ~50 ms crowd frame:
  **93.6-94.0% hit rate within a frame, and still ~2,000 misses copying 74-77 MB every
  frame**, `streams` 11.3-11.7% = 5.6-5.9 ms, with **95-97% of the copied bytes
  repeating the previous frame's key**. Vertex bindings 61-63 MB, dependent fetches
  11 MB, index buffers 1.8 MB. So it is real copying, of the same buffers, thrown away
  at every frame boundary — the fix is the cache's LIFETIME, and it is worth ~11% of a
  crowd frame.
  Half the ambiguity had in fact been answerable by reading nine lines, which is worth
  saying because the plan said it was not: `ProfScope(&g_prof.streams)` wraps only the
  `CopySwapped`, so a cache hit never touched that column at all and its lookup was
  charged to `other` the whole time. Gotcha 233 is the general form — a hit RATE and a
  byte COST are different questions, and 94% sounds like the copying is gone.
* **The content check needed a control, and the control changed the answer.** A
  persistent cache is only correct if the bytes at a repeated key are the same bytes, and
  level 2's hash comparison read **100.0%, in every window of every run**. That is
  indistinguishable, from the output, from a comparison whose two sides are equal by
  construction. `CZ_VK_STREAM_CENSUS_POISON=1` salts the hash with the frame number so
  identical bytes must hash differently: it reads **0 of 96,048 (0.0%)** on the same
  binary, against 75,492 of 75,492 unpoisoned. With the control in place the unpoisoned
  arm reads honestly too — **164 of 10,154,820 repeated keys DO change content**, a
  recurring set of ~26 — so the design conclusion moves from "safe to cache blindly" to
  "must invalidate". Gotcha 234, which is gotcha 30 in the shape it takes for equality
  checks, where it hides best because the passing state is silent.
* **The arena growth ran inside `DoDraw`.** `BeginFrame()` is called from there, and the
  growth's `vkDeviceWaitIdle`, allocation and map were charged to the draw path's
  `other` — the 29.8% single-frame spike the operator session measured. Moved to the end
  of `DoSwapImpl`, after the fence wait, which satisfies the old comment's own stated
  safety condition more directly than the old site did. **Stated as a measurement fix,
  not a performance one**: verified against the pre-registered prediction that the
  black-frame count per growth is UNCHANGED at one, and it is (pre-move: exhausted 1817,
  black 1818, 6 of 13,410; post-move: exhausted 1258, black 1259, 7 of 5,387 — the same
  four early-boot frames otherwise, at identical draw counts). The frame that overruns is
  lost either way. Closes the last live piece of open-items 1c.

**Gates:** `--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**; `truncated=0`;
`no translated shader` = 0; both PM4 capture oracles clean (24,527,474 packet lengths
agreeing, every indirect buffer tiling exactly). The picture was not re-checked against
capture E — nothing this part touches what is drawn — and that is an argument rather than
a measurement, as it was in part 20.

**What this part deliberately did NOT do.** The cross-frame cache itself. It needs
storage that outlives the frame, an invalidation mechanism that is not hashing (the
candidate is guest-page write tracking), and counters for both; the hand-off says so and
sizes it at a session. Measuring first and stopping is what the plan asked for, and the
0.0016% mismatch is exactly the fact that would have been discovered late and expensively
by writing the cache first.
