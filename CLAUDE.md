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

The full list lives in the Fable 2 and Asura's Wrath CLAUDE.mds and is not duplicated
here. The ones that have already proved relevant to *this* title, plus the ones that are
about to:

1. `mftb` is compiled to `__rdtsc()` — force-include a 49.875 MHz timebase over PPC TUs.
2. XenonRecomp ends functions at `bdzlr`/`bdnzlr` (conditional returns!) — a truncated
   CRT memset corrupts silently; fix with `[main].functions` size overrides.
3. **Jump-table mis-detection — and a zero is a detection failure, not a fact.**
   XenonAnalyse found **zero** tables in this 8.8 MB code section; our scanner found
   **232**. Never trust a zero, and hand-audit anything the scanner is unsure of. A
   missed table emits `return;` without restoring non-volatiles.
4. All optimization flags OFF until a run works.
5. Kernel stubs must fail honestly, never fake success (Fable 2's XMA context bug cost
   weeks). Corollary: a stub that returns an error but leaves its **out-parameter**
   untouched is worse than no stub — the guest often ignores the status and reads the
   buffer anyway.
6. The alias/weak-link seam (`PPC_FUNC` overriding `__imp__sub_X`) is how all hooks and
   whole-image probes are built.
7. A probe expensive enough to stall the game manufactures the stability it reports —
   every instrument needs its own control.
8. `log_level=3` is the *minimum* for named kernel calls in Xenia, and still not the whole
   surface: `kHighFrequency` exports need `log_high_frequency_kernel_calls=true`, which
   defaults off and hid 40 of 288 imports on Asura's Wrath.
9. Put the guest arenas where the console puts them, **and round every size the kernel
   reports the way the console rounds it** — the guest's own heap manager builds its map
   from those numbers.
10. The image is the authority on what a title imports — `ppc/ppc_recomp_shared.h`, not
    the previous port and not the capture.
11. A recompiler's `default: __builtin_unreachable()` converts every static-analysis gap
    into an arbitrary-code jump. Already patched in the shared XenonRecomp checkout.
12. A function-coverage capture is a **two-sided** oracle: forwards recovers missing entry
    points, backwards localises a control-flow divergence. Treat its function boundaries
    as ranges, never identities.
13. A capture request is a hypothesis with a shelf life — re-read it against the current
    ledger before running it, and re-read the delivered *notes* against the ledger before
    believing their conclusions.
14. Adjacency in *our* arena is evidence about our allocator, never about the title.

New here, and transferable to any XBLA port:

15. **`encryption = 1` does not say which key.** Xbox 360 XEXs use either the retail key
    or the all-zero devkit key, and nothing in the header distinguishes them. XBLA titles
    commonly use the devkit key; this one does. Stock XenonRecomp hardcodes retail and
    **returns an empty image with no diagnostic** — base 0, size 0, zero sections — which
    reads as a broken TOML or a corrupt file, not as a wrong key.
16. **A container's "compression" field can mean a codec, or a table of zero-runs, and
    the wrong reader does not error.** `compression = 1` (basic) is a list of
    `(data_size, zero_size)` pairs; `compression = 2` (normal) is LZX. `decrypt_xex.py`
    reads a normal-compression header as a basic block table and produces a confident,
    entirely fictional block list. Both template ports would have accepted it silently.
17. **A long register-save ladder contains a short one.** `__savevmx_64`'s 46th rung is
    `li r11,-0x120` followed by 17 more pairs and a `blr` — an exact match for an 18-pair
    `__savevmx_14` scan, at an address 0x170 *inside* a function the same scan just
    called 516 bytes long. What caught it was the cross-check that the four vector
    ladders must be **contiguous**; the individual matches all looked fine.
18. **One title can use two different base registers for its ladders.** The gpr ladders
    here are r1-based, the fpr ladders r12-based. Asura's Wrath's notes warn the base
    register varies between titles; it varies within one too.
19. **v14–v31 use classic VMX encodings, v64–v127 use VMX128** (`stvx` = opcode 31,
    `stvx128` = opcode 4), and VMX128 spreads the 7-bit register number across the
    instruction so the high bit lands in bit 2 of the low half. A matcher that doesn't
    mask that bit stops halfway through the 64-rung ladder — at register 96, which looks
    like a plausible ladder length.

From round 1's captures (details in the findings ledger):

20. **An emulator's licence state is part of the capture config, and its default is
    wrong for a paid XBLA title.** Xenia's `license_mask` defaults to 0 → the game boots
    its **trial**, which is not a subset of the full game (finding 1). Nothing in a disc
    title's methodology catches this.
21. **A coverage trace's "functions" include every branch target the emulator saw
    executed** — so a recovered jump table's case labels arrive looking exactly like
    undiscovered entry points. Here 870 of 1,090 candidates. Adding one **splits the
    switch's parent**, turning its remaining cases into bare `return;` — the exact defect
    the repair tool exists to fix, reintroduced by the tool meant to improve coverage,
    and the two then fight to a stable non-zero error count instead of converging
    (finding 5a). Two "functions" 4 bytes apart sharing an end address are one function.
22. **An analysis image has a *stage*, and a byte pattern is only valid for one stage.**
    XenonRecomp overwrites every import thunk with `nop;nop;nop;blr` during load, so a
    scanner looking for the on-disk `mtctr r11; bctr` form finds **zero** in a dumped
    loaded image — and zero silently promotes all 244 thunks to "missing functions"
    (finding 5b). Asura's Wrath never hit this because its image came from
    `decrypt_xex.py`, which never runs the loader.
23. **A tool that reports convergence is making a claim; check it against the thing it
    claims to have fixed.** `fix_switch_function_bounds.py` printed "0 new this round"
    through a whole fixpoint loop while three errors persisted, because it computed a
    function's end from the *widened* start and emitted entries that ended where the
    real function began (finding 5c).
24. **Check a log's line shapes before filtering on one.** Xenia emits `d>` `i>` `G>`
    `A>` `!>` `F>` and unprefixed continuations. `VdSwap` is logged at `i>`, so a
    `d>`-only filter reports `VdSwap = 0` — a clean, small, wrong number that reads as
    "this title never swaps" (finding 4).

From round 2 (closing phase 0.1):

25. **A grep that cannot match is not a clean result.** The bootstrap claimed "zero
    `// ERROR:` comments in the generated code". The recompiler emits `// ERROR {:X}` —
    **no colon** — so the pattern never had a chance to match and the check was never
    run. The true count was 31. Before believing a zero, confirm the pattern can match
    *something*; the cheapest version is to grep the emitter, not the output.
26. **A dropped direct branch is an unimplemented instruction wearing a different hat.**
    When a branch target is not the exact start of a known function, XenonRecomp emits a
    bare `// ERROR <addr>` comment and nothing else. No stdout diagnostic, exit 0, and
    the C++ compiles — the control transfer just never happens. Measure it with
    `tools/find_dropped_branches.py` (finding 13).
27. **The direction of a dropped branch names the defect and the repair, and they are
    opposites.** Backward → a loop header was declared a function and split a real one;
    remove the start. Forward → the function was truncated before its outlined cold
    block; widen it. Applying either repair to the other class makes things worse.
28. **The coverage oracle's mid-body trap is not limited to switch labels.** A loop
    header is a branch target too, appears in no switch table, and passes every
    case-label filter. Nine got through here. No pre-hoc heuristic separates them from
    genuine indirect-call targets — two of the nine looked completely ordinary — so the
    coverage tool *proposes* and the dropped-branch check *disposes*. Run them in that
    order, always (finding 5d).
29. **An "implemented" instruction can still be impossible.** `VADDUWS` emitted
    `simde_mm_adds_epu32`, which does not exist in simde and has no SSE equivalent at any
    level. It had presumably never been exercised. A recompiler case is only proven by a
    title that uses it *and* a compile that consumes it (finding 14).
30. **A vector test that has never failed has not been shown capable of failing.** Vector
    lowering hides two invisible conventions — the whole-vector byte reversal (which
    swaps pack operand order) and saturation edges. Both are silent wrong *values*, not
    crashes. Write the differential test against scalar PPC semantics, then break the
    implementation on purpose and confirm the test screams.

From phase 0.2 (the first compile):

31. **The recompiled image needs Clang, not a preference but a requirement.**
    `ppc_context.h` builds `PPC_FUNC_PROLOGUE()` on `__builtin_assume`, which GCC has no
    spelling for — so GCC fails in *every one* of the 57,822 function bodies. Select the
    compiler **before** `project()`; after it, CMake has already locked one in.
32. **A force-included `#define __rdtsc()` breaks the system headers, not the guest.**
    The `mftb` shadow must not be in scope when `<immintrin.h>` declares its own
    `__rdtsc(void)`, or the build dies inside a system header with an error naming
    neither this project nor the guest. Include `<x86intrin.h>` first to set the guard,
    then `#undef`/`#define`.
33. **`PPC_FUNC` vs `PPC_FUNC_IMPL` is a linkage difference that only surfaces at link
    time.** `PPC_FUNC_IMPL` is `extern "C"`; the image's references are C++-mangled,
    because `ppc_recomp_shared.h` declares imports with a plain `extern PPC_FUNC(x)`.
    Define kernel-import stubs with **`PPC_FUNC`**. Guest functions escape this only via
    their `__attribute__((alias(...)))` weak alias — the same seam every hook uses.
34. **The save/restore ladders are not missing symbols.** All 236 are declared in the
    shared header and called everywhere, and XenonRecomp defines them itself as
    `__imp____savegprlr_14` with a weak alias. Stubbing them gives 236 duplicate symbols.
    Only `__imp__`-prefixed names are genuinely undefined.
35. **Link the image with `--whole-archive` for the phase 0 gate.** A normal
    static-library link pulls in only referenced objects, so an unreferenced TU carrying
    an undefined symbol links cleanly — and the gate passes while proving nothing.

From phase 0.3 (the `.xtr` decoder, closing finding 10):

36. **A capture's notes prescribe a *method*, and skipping it manufactures a finding
    about the title.** Gotcha 13 says to re-read a capture's notes before believing
    their *conclusions*; this is the other half. B1/B1b's notes said to align over the
    fixed boot+movie prefix and ignore the idle tail. Comparing whole runs instead
    produced "16% of frames agree — NOT content-deterministic", a confident claim about
    the game that was purely an artifact of including 619-vs-409 frames of *a human
    deciding when to press exit*. Same data, correct window: 0.42%.
37. **An emulator-side bookkeeping field can be deterministic in aggregate and
    non-deterministic per frame.** `MemoryRead` counts agree to 0.37% in total and align
    on only 17.7% of frames, because Xenia's dirty-tracking decides *when* to record, not
    *whether*. Folding them into a content fingerprint dropped frame agreement from 42.7%
    to 16.0%. A field's total being stable is not evidence that its distribution is.
38. **Gate on per-era aggregates, never on absolute frame index.** Even over the correct
    window, frame-exact agreement between two *hardware* runs is only 80.0% — phase drift
    concentrated at lag +3. A frame-indexed GPU gate would report ~20% divergence against
    a correct renderer.
39. **One opcode can be recorded differently from all the others, and the shape of the
    disagreement names the cause.** Spread across many opcodes → the bit layout is wrong.
    All of one and none of any other → that opcode is special. `INDIRECT_BUFFER` is stored
    one dword short (the size lives in the following `IndirectBufferStart`), so a replay
    tool that trusts either length feeds the command processor a malformed packet.
40. **An unexercised bounds check is not a working one.** The ported `.xtr` `step()`
    computed a next-offset past EOF without checking it. B1 never triggered it; B1b did,
    on the first run, because these captures stop mid-command. "It has always worked" was
    a statement about the inputs.
41. **A check that always fires is a check people learn to ignore.** The response to a
    known-benign alarm is to encode the knowledge (`xtr.PM4_SHORT_RECORDED`), never to
    widen the tolerance — widening it also silences the unknown cases it was built for.

From phase 1 (the runtime; details in `docs/phase1-notes.md`):

42. **A generated stub cannot obey the out-parameter rule, and the fix is to say so.**
    "A stub must fill its out-parameter or not exist" is right, and a generator working
    from a list of *names* has no signature to obey it with. Guessing by name prefix
    would be wrong silently. State the limit in the generator, name the failure modes,
    and keep the escalation short — a real signature in `imports.cpp`. Two recognisable
    symptoms: a guest trusting an untouched out-buffer (corruption far from the call),
    and **a segfault on or near `0xC0000002`, which always means an unimplemented import
    was asked for a pointer** rather than an NTSTATUS.
43. **A ported diagnostic can carry an assumption about the memory map it came from.**
    Both template ports bound their guest-stack scans with `addr < 0x80000000`. Our
    guest thread blocks come from the o1heap arena at `0x88000000`, so the bound failed
    on the first word and every stall trace printed `callers:` with nothing after it —
    an instrument that looks like it ran and found nothing. Gotcha 25 in a new place.
    Every inherited constant that is a *range* deserves the question every inherited
    *address* gets.
44. **When the safe value is a property of the generated function list, compute it from
    the function list.** Minted export thunks must land inside the dispatch table's
    CODE range. Asura's Wrath hardcoded an address that was 4.7 MB outside it and the
    stub never ran (its finding 54); Case Zero has no free constant at all — 16 bytes
    between its last mapped function and the end of the range. Scanning for the longest
    unmapped run costs one pass per process and cannot go stale.
45. **Two captures of "the same drive" need not be nested.** A5 is A1's drive with
    high-frequency logging on, and it is *nearly* a superset — but 11 names appear only
    in A1, the whole storage-device-selector path that drive did not enter. Treating the
    richer capture as authoritative everywhere manufactures divergences.

46. **The contract of an async call includes its notification, not just its data.**
    Our `NtReadFile` filled the buffer, filled the `IO_STATUS_BLOCK` and returned
    success — and hung the boot, because this engine signals completion through the
    **APC routine** and passes `event = 0`. Dropping the APC left every observable
    looking correct, which makes it *harder* to find than an outright failure. Two
    sub-traps: the APC routine's low bit is a kernel flag, not part of the address,
    and its `IO_STATUS_BLOCK` argument is a **guest** address, not the host pointer
    the marshalling layer hands you.
47. **A `kHighFrequency` export can be the one that matters.** The read above is
    invisible in A1 and in every other level-3 capture; only A5 shows it. A capture
    set needs at least one high-frequency arm or its quietest exports are unfalsifiable.

48. **"Fill your out-parameters" is only safe on top of a correct signature.** Writing
    an out-parameter a call does not have turns a harmless leftover register into a
    wild store — and it fails inside your own kernel, in the very place the rule
    exists to protect. `NtClearEvent` takes one argument; `NtSetEvent` takes two.
    Check arity against a capture before adding the write.
49. **Device-struct offsets are per-title; the image states its own.** The previous
    port reads the GPU ring's kicked-write-pointer mirror at `dev+11088`; Case Zero's
    is at `dev+10956`. Both are recoverable from the single store to `CP_RB_WPTR`
    (`0x7FC80714`) in the image. Copying the other port's number reads an arbitrary
    field as a ring position.
50. **Against an intermittent failure, an arm is not a measurement — a rate is.** A
    single-run A/B over a nondeterministic crash confidently named the arm that
    happened not to fire, and produced a clean, decisive, wrong conclusion. Re-run at
    a longer duration, every arm survived. Gotcha 7's other half.

51. **A rate measured once is a fact about that afternoon.** The same binary and drive
    measured ~2 crashes in 10 one session and 6-7 in 10 the next. Re-establish the rate
    whenever anything around an intermittent fault changes — including the instrument
    you added to watch it.
52. **Three page-0 policies turn a null fault into a diagnosis.** Trap-everything says
    only "something touched null". Allowing null *reads* (the console's behaviour) and
    re-running says whether it was a read or a write, and mapping page 0 fully says
    whether the pointer was merely null or genuinely wild. Here the faulting address
    walked `00000000` -> `00000002` -> off the end of the guest space, which rules out
    the benign console-tolerated null read and names a bad base pointer instead.

From finding 28 (the XAM surface):

58. **A stubbed *query* can steer the whole boot, and one value can be worth dozens
    of implementations.** `XamGetSystemVersion` is a feature gate in seven places;
    as an honest-failure stub it returned `0xC0000002`, which compares ABOVE every
    threshold, so the title took the dynamic-import branch at all seven and asked for
    XAM ordinals we do not have. Returning 0 — "this system does not have those newer
    entry points" — is the truthful answer AND the one the capture shows. Raise a
    version gate only together with the exports it unlocks.
59. **A predicate-shaped import has no honest failure value — gotcha 5's blind spot.**
    `RtlCompareStringN` returned `STATUS_NOT_IMPLEMENTED` and the guest tested it with
    `cmpwi r3,0`, so `0xC0000002` was not an error it could notice: it was a valid
    answer meaning "not equal". Every string comparison in the title silently returned
    "different". When a return value is a comparison or a boolean, "fail honestly" is
    not available and implementing it is the only correct option.

From findings 30-32 (the profile, the version gate and the notification queue):

60. **When the capture cannot show a structure, the guest's own walk of it can.** No
    Xenia log records what an import *wrote*; it prints pointer arguments as they were
    before the call. But the code that reads the buffer back names every offset —
    `sub_825E4E88` gave us `+16` = setting id, `+32` = value, stride 40 — and the sizes
    the title itself computes (3 settings -> 0x80, 2 -> 0x58) pin the header at 8
    bytes. Two witnesses, neither of them a guess about the SDK.
61. **A predicate stub is loudest where it is quietest.** `XNotifyGetNext` returning
    `0xC0000002` reads as TRUE, so the title believed a notification had arrived on
    every one of ~10,000 polls a boot and read the id and param out of stack slots the
    stub never touched. Gotcha 59's rule again — and the corollary is that a
    predicate's out-parameters must be filled on the FALSE path, which is the path
    taken thousands of times.
62. **An inherited constant stops being free the moment the image branches on it.**
    `XboxKrnlVersion` was 2.0.14448.0 in both template ports; Case Zero's rumble path
    tests `build < 5611` and Xenia's config says 1888, so the capture takes a legacy
    branch our value skipped. Matching the capture is what a gate means. And the
    change was A/B'd rather than asserted: 82/85/82 vs 85/82/85 over three runs each,
    i.e. no measurable difference — one arm per value would have "proved" a win that
    is not there (gotcha 50).

From finding 33 (the position-57 divergence, closed — and it was not the DVD cache):

63. **A probe answers the question you point it at, and pointing it downward
    confirms the symptom.** Our boot probed `\Device\Image`, then
    `\Device\Harddisk0\partition0`, then compared a path against `"cdrom0:"` — a
    coherent DVD-cache trail, every reading accurate, and the whole subsystem
    innocent. What found the cause was walking OUTWARD to the first caller whose
    behaviour differs from the capture. The cheapest form of that question is not
    "why did this fail" but **"does hardware even get here"**, and one grep of A1
    answered it: hardware never opens `\Device\Image` at all.
64. **A one-instruction tail-call thunk hides an import from its caller's
    disassembly.** `sub_825D7A50` is literally `b XamContentGetLicenseMask`, so the
    deciding branch reads as `sub_825D7A50()` and the import name appears nowhere near
    it. When a call site's callee is a 4-byte function, look through it.
65. **When a predicate's polarity will not hold still, print it.** `sub_82829098` has
    four routes to a zero return and two of them are failure paths; three static reads
    gave three answers. One instrumented run gave the right one. Printing costs less
    than reading it twice and cannot be wrong.

From finding 34 (the XAM message/task block):

66. **A dispatcher's real interface is the set of messages its callers can send, and
    that set is in the image.** Replaying the `r3`/`r4` setup before all 18
    `XMsgStartIORequest`/`XMsgInProcessCall` call sites recovered all 25 `(app,
    message)` pairs in one pass — far better than discovering one per run. Then check
    the static surface against a run: the boot sends exactly two, both handled, zero
    unknown. Static scan and dynamic check agree, which neither proves alone.
67. **An implemented import that has never been reached is a prediction, not a
    result.** Eight of finding 34's nine are guest-derived, none is a guess, and none
    has executed. Gotcha 30 applies to code as much as to tests — say which ones ran.
68. **Adjacency in a first-occurrence gate is not causation — check the thread id and
    the line number.** A first-occurrence gate flattens a multi-threaded timeline into
    one sequence, so two positions sit next to each other whenever nothing *new*
    happened in between, however far apart and however unrelated they are. Position 84
    (`MmMapIoSpace`, audio thread, A1 line 54,145) and position 85
    (`XamShowDeviceSelectorUI`, frontend thread, A1 line 111,694) are 57,500 lines and
    two threads apart; calling the first a blocker for the second was wrong. One grep
    for the line number and thread id settles it before any work is planned on top.

From finding 35 (the early `RtlNtStatusToDosError`, closed):

69. **A pseudo-handle is a constant, not an address, and a handle scheme built on
    addresses cannot tell the difference.** `GetCurrentThread()` returns `0xFFFFFFFE`,
    which has bit 31 set and therefore sailed through our "a handle IS the object's
    guest address" check before being rejected as dead. The scheme already excluded
    `0xFFFFFFFF` explicitly — it knew about -1 and not -2, which was the entire bug.
    Any runtime encoding handles as addresses inherits this.
70. **Grep the captures for the offending *constant*, not just the import the
    backtrace named.** `NtDuplicateObject(FFFFFFFE)` occurs twice; the same constant
    reaches `ObReferenceObjectByHandle` **eleven** times, where our identity mapping
    was handing `0xFFFFFFFE` back as an object pointer. Fixing only the site the
    backtrace pointed at would have left the more frequent and more dangerous one
    wrong, and silent.
71. **One displaced import can manufacture several gate windows.** A5 showed three
    real mismatch windows; two of them were the wake of this single early call, and
    fixing it turned them into recognised permutations. Count causes, not windows.
From finding 27 (the null base pointer, resolved — and it was none of the above):

53. **A scanner's own count is not a measurement of the thing it scans for.**
    `find_jumptables.py` reported 232 tables through all of phase 0; the true number
    was 234. A scanner reports what it found and is structurally incapable of
    reporting what it silently rejected — here, two tables whose `cmplwi` bounds check
    the compiler had hoisted outside the search window, leaving no case count. The
    check has to be an independent question asked against the image, which is
    `tools/find_unlowered_switches.py`. Gotcha 3's rule ("a zero is a detection
    failure, not a fact") applies to *every* number a detector prints, not just zero.
54. **An unlowered `bctr` is the third silent defect class, after unrecognized
    mnemonics and dropped branches — and the quietest.** XenonRecomp emits
    `PPC_CALL_INDIRECT_FUNC(ctr); return;`, which calls the case body and then returns
    with no epilogue. Nothing is printed, it compiles, and the case body even computes
    the right answer because it gets the same `PPCContext`. The only symptom is that
    the caller resumes with the callee's r14..r31 — and the crash lands frames away,
    on a value that looks like a null pointer from our kernel and is not.
55. **A tool that recovers missing entry points can turn a loud failure into a silent
    one.** The missed table above should have crashed *at the dispatch*, because
    `PPC_CALL_INDIRECT_FUNC` on a case-label address normally finds nothing and jumps
    to null. It did not, because the coverage oracle had already added those labels as
    functions (gotcha 21), so the dispatch resolved and the damage became invisible.
    Gotcha 21 said adding a case label splits its parent; this is the other half.
56. **An "intermittent" crash can be perfectly deterministic.** Six crashing runs here
    were **byte-identical** — same registers, same host pc, same stack pointers. Only
    *whether the boot reached the site within the timeout* varied. Diff the crash
    reports before theorising about races; it costs nothing and it reframes the hunt.
57. **In a recompiled frame the register dump is stale and the host pc is not.** The
    compiler keeps `PPCContext` fields in host registers between calls, so `r3`/`r4`
    at the fault named the wrong objects entirely. `addr2line` on the RAW host pc gave
    the exact `ppc_recomp.NNN.cpp` line. The same staleness in `ctx.r1` puts one extra,
    already-returned frame on top of the guest back-chain — read frame #0 as advisory.

From finding 36 (the audio driver, and the first fully clean A5 gate):

72. **A callback's argument can be a pointer to the context, not the context.**
    Case Zero's registered render-driver callback is `lwz r3,0(r3); b <body>` — it
    dereferences before doing anything, so a driver that passes the registered
    context straight through hands the body a pointer one indirection too shallow.
    The body then reads its wait objects and its ring buffer out of whatever sits
    there. What proves it is deliberate is the object's own constructor: the
    registration is slot 3 of a SECOND vtable, so its `this` is `obj+4` and the
    `addi r9,r31,-4` that builds the context recovers `obj+0` — while the body
    measures every field from `obj+0`. Body wants obj, registration supplies obj,
    thunk does one load.
73. **A guest-supplied pointer must never be dereferenced without a check, however
    impossible the null looks.** `WaitDispatcher` read `header->Type` unguarded, so
    ANY guest passing a null dispatcher object to ANY `Ke` wait took the host down
    at address 0 — and the crash reporter labels that "outside the guest space, a
    host-side bug", a true statement that names our kernel rather than the guest
    that provoked it. On hardware it would bugcheck, so there is no faithful
    behaviour to copy; there is only the rule. Corollary: do not *poll* an unusable
    object either — this call site is an infinite wait, so polling wedges the thread
    for the life of the process.
74. **An allocation the runtime owns must get out of the guest's way.** Taking the
    XMA context array from the bottom of the physical arena moved the title's own
    447 MB reservation by 20 KB — a change to an unrelated subsystem bought for
    nothing (gotcha 9). A1 shows Xenia doing the opposite: the title gets physical
    0x03D93000 and the context array sits one page past the END of that reservation.
    `AllocPhysical` grew a `topDown` flag.
75. **A duration is part of a gate's configuration, and "not reached" is not "not
    reachable".** At 30 s the A5 gate stopped at position 114 and the XMA path
    looked blocked. The same binary reaches 119 at 90 s. Before theorising about
    what is blocking a gate position, run it longer — and then run it again:
    gotchas 50-51 are usually quoted about crashes, but they are really about single
    runs of anything. How far a multi-threaded boot gets in a fixed wall time is a
    distribution. One 90 s run reached position 84 and the next did not; the rate is
    5 of 7.
76. **The XMA decoder's register file is console knowledge, and this title drives it
    directly.** `0x7FEA0000`, LITTLE-endian (`lwbrx`/`stwbrx` on a big-endian
    machine): `+0x1800` holds the context array's PHYSICAL address, `+0x1A80` is a
    one-bit-per-context kick bitmap indexed `>> 5`. Contexts are 64 bytes and the
    index is `(MmGetPhysicalAddress(ctx) - base) >> 6`, so they must come from one
    contiguous array at the address you published. Publish it big-endian and every
    index is nonsense, silently — the index only ever picks a hardware bit.

From finding 37 (the frontend was waiting for input, and the arm that proved it):

77. **"Finished and waiting for a human" and "stuck" are the same picture from
    outside.** A boot that has reached its title screen shows a steady frame rate, a
    flat file count and an unchanging kernel-call profile — and so does one that has
    deadlocked there. No passive instrument separates them, because the difference is
    not in what the program is doing, it is in what it is waiting for. The only
    discriminator is to supply input and see whether it moves.
78. **An instrument that manufactures progress has to be loud.** `CZ_FAKE_START_MS`
    presses START for the guest, and a run that quietly had it on would show the boot
    advancing past the title screen and invite the conclusion that the import you just
    wrote unblocked it. Off by default, announces itself on every press, and never on
    for a gate run. Same discipline as `CZ_NO_AUDIO_PUMP`, higher stakes: this one
    fabricates *evidence*, not just behaviour.
79. **Re-measure your own premise before acting on it, not just the capture's.**
    Gotcha 13 says a capture request is a hypothesis with a shelf life. So is a note
    in your own status section. "Our run reaches `mainmenu.tex` and stops" was true
    when written and had quietly stopped being the interesting fact: the runtime now
    loads 64 files through to the title screen's 3D scene and renders it at ~34 fps.
    (The "64" is itself the print cap — see gotcha 109; the real count is 84.)
    Acting on the stale version would have sent someone hunting a file-loading bug
    that no longer exists.

From finding 38 (the load stall — traced end to end, not yet fixed):

80. **A GPU capture records every packet's own length, which makes it an oracle for
    your command processor's arithmetic.** Xenia's `.xtr` writes a
    `PacketStart {base_ptr, count}` per packet, `count` being the boundary hardware
    actually used — inside indirect buffers included. `tools/pm4_packet_lengths.py`
    checks our rule against all 24,527,474 of B1's and prints the opcodes that
    disagree. It cost an hour to write and it retired, in one run, a fix I had
    already made, a buffer-dump analysis that appeared to confirm it, and two
    follow-up theories. Write the oracle before the theory.
81. **The instrument that is missing is the one whose silence you read as health.**
    Our wait trace covered `NtWaitForSingleObjectEx` only. The Draw Thread waits on
    four handles in `WaitForMultipleObjectsEx`, so the single most important wait in
    the title was the one wait the trace could not see, and its absence from the
    stall dump read as "that thread is fine". Half a wait surface answers "who else
    is stuck?" with silence that looks like "nobody".
82. **A thread stuck without being in any kernel wait is spinning in guest code, and
    nothing inside the runtime can see it — it is not calling you.** An outside
    debugger can, but only if its host thread ids can be joined to your guest ones.
    One always-on line per thread (`guest thread tid=... host tid=...`) is what turns
    `gdb -p` from anonymous stacks into named guest functions.
83. **The address a driver REGISTERS with the kernel need not be the address it
    POLLS.** Case Zero registers `block + 0x3C` with `VdEnableRingBufferRPtrWriteBack`
    and dereferences `block + 0` in its free-space wait. Publishing the read pointer
    honestly into the registered slot therefore satisfies nobody. A hardware
    watchpoint on the polled word in a HEALTHY run names its real writer in one hit —
    do that before reasoning about what "should" write it.
84. **A parser that stops early must say so.** "A packet claiming more than the buffer
    holds: stop, do not guess" is the right policy and it was silent, so the defect it
    was detecting looked like nothing at all. What it was dropping was the last packet
    of the buffer — which in this title is the driver's ring-progress fence, i.e. the
    one packet whose loss parks a thread forever.
85. **When a walk desyncs, the position it fails at is data, not the bad packet.** The
    header in our first truncation report was `3F800000` — the float 1.0. Keep a trail
    of the last few packets and dump the untrusted tail, or every report points at an
    innocent dword.
86. **The control for "did my change do this" is the old binary run NOW, not the old
    binary's remembered numbers.** The A1 gate started permuting positions 71-73 in
    half our runs where the committed binary had been 6-of-6 clean, which reads as an
    unambiguous regression. `git stash`, rebuild, re-run: the committed binary does it
    too, and with both binaries built side by side and runs ALTERNATED, the committed
    one permuted 1 of 6 and the new one 0 of 6. Those positions were always
    scheduling-sensitive. Gotcha 51's corollary — and acting on the remembered number
    would have reverted a correct change to fix a defect that did not exist.

From finding 39 (the stall was our own `VdSwap`, and it presented as a parser bug):

87. **A kernel export that writes into a guest-supplied buffer owns the whole
    reservation, not just the part it has content for.** Our `VdSwap` wrote 12 dwords
    of a 64-dword block and returned; the caller advances its write pointer by the
    full 64 whether or not the kernel fills it (`addi r11,r29,256`, `r3` never read),
    so the 52 dwords we left alone were submitted as packets. Command buffers are
    recycled, so those dwords were the *previous frame's* command stream — real
    headers at wrong offsets. Hardware fills them with 52 × `0x80000000`, a type-2
    one-dword no-op, the only encoding that crosses arbitrary dwords uninterpreted.
    Gotcha 5's other edge: a stub that does *part* of a job leaves a hole shaped like
    real data, and it surfaces in a subsystem it was never near.
88. **An oracle for your arithmetic does not clear your inputs, and a clean parser
    gate is not a clean parser.** Two capture-derived gates said our command
    processor was right — every packet length against hardware's own boundaries on
    24,527,474 packets, and every indirect buffer's start address and internal
    boundaries chained on all 28,726 — while it desynced dozens of times a minute.
    Both were true. The bytes were wrong. When every check of a computation passes
    and the result is wrong, stop checking the computation.
89. **A capture is a vocabulary, not just a transcript.** B1 uses only **225 distinct
    packet headers** across 24.5 M packets. Walking a suspect buffer and flagging the
    first header that is not in that set put the desync at one place — immediately
    after `XE_SWAP`, in all six dumped buffers — after six truncation reports had each
    named a different innocent position. A tight vocabulary is a cheap, extremely
    strong classifier, and any format with a capture has one.
90. **Check what a distinctive record *is* before reasoning from it.** B1's single
    zero-header packet, which finding 38 used to reject the zero-as-no-op reading, is
    an `INDIRECT_BUFFER` whose header the trace records as zero — count 2 (the short
    recording 0x3F always gets), an address in word[1], and `IndirectBufferStart` with
    that base as the next command. So the capture contains **no** genuine zero header
    and is silent on the question, which two sessions treated as settled in opposite
    directions. One anomalous record deserves its own identification, not a conclusion.
91. **A change that measurably helps can still be the wrong change.** Reading zeros as
    no-ops walked two of six broken buffers to completion — a real, reproducible,
    partial improvement — because our unwritten tails were zeros wherever the heap
    happened to be fresh. It addressed the *shape* of the corruption, not its source,
    and the A/B over stalls duly showed nothing (3 of 10 vs 4 of 10). Partial
    improvement with no effect on the symptom is a signature worth recognising.

From finding 40 (the 1-in-40 crash, re-measured to zero — and the barrier hole found
on the way):

92. **A barrier that compiles to nothing is a COMPILER bug, not a hardware one, and
    x86's strong ordering is the trap.** XenonRecomp lowered `sync`, `lwsync` and
    `eieio` to `// no op`. For the hardware half that is right on x86-64 — TSO already
    gives every ordering `lwsync` promises — which is exactly why it looks safe. It is
    not: every guest access is a plain C++ load/store through `base`, and a construct
    that emits no code constrains clang not at all, so at `-O2` "fill the buffer,
    lwsync, publish the index" may become "publish the index, fill the buffer". Fix
    them differently or get it backwards: `lwsync`/`eieio` -> `atomic_signal_fence`
    (compiler barrier, no instruction), `sync` -> `atomic_thread_fence` (a real fence,
    because store-load is the one ordering x86 does NOT give).
93. **An inherited crash rate is a hypothesis about a binary that no longer exists.**
    Task #11 recorded 1 crash in 20-40 runs; the same test on the current binary is
    **0 of 20**, because finding 39 changed what a run spends two minutes doing.
    Gotcha 51 says a rate is a fact about an afternoon; this is the stronger form —
    re-measure before you characterise, or you will spend a session hunting a fault
    the code no longer has.
94. **A diagnostic can be silent on precisely the case it was written for.** The crash
    reporter's "LIKELY null indirect call" test required `si_addr == nullptr`, which is
    never true when `ctr` is *zero*: the dispatch-table lookup is never reached, so the
    fault address is whatever the lookup computed. The one shape it existed for was the
    one shape it could not see. Widening it is one line; proving it needed
    `CZ_CRASH_TEST=nullcall`, because gotcha 30 applies to diagnostics as much as tests.
95. **A long rate run leaves behind a free control.** Twenty 120-second boots produce
    twenty complete logs, and every log-based gate you own replays over them for
    nothing. Four hand-run A1 gates said 3-of-4 permuted where the session had earlier
    seen 2-of-2 clean — an obvious regression, and wrong. Gating the 20 saved logs per
    arm gave **13 clean / 7 permuted on BOTH binaries, identically.** Gotcha 86 says
    the control is the old binary run now; this is the cheap way to have run it.
96. **If you have written a tool twice from scratch, keep it the first time.** A PPC
    disassembler over the loaded image was hand-rolled and discarded in two consecutive
    sessions. It is `tools/gdis.py` now. The host toolchain genuinely cannot do this —
    `objdump` has no PowerPC target here, `llvm-objdump` has no `-b binary`, and
    `llvm-mc` loses instruction alignment at the first unknown VMX128 encoding and
    keeps printing plausible garbage after it.

From finding 41 (the critical-section yield spin):

97. **A "yield loop" is a busy loop wearing a polite name, and it bills to the half of
    the profile nobody reads.** `sched_yield()` on an otherwise-idle multicore host
    returns immediately — there is nothing to yield to — so `while (!cas) yield();`
    burns a full core. Case Zero blocks two threads forever by design (on console they
    just sleep), and ours spun: **317% CPU, of which 85 of 106 seconds were SYSTEM
    time.** User-time profiling shows those threads computing almost nothing, which is
    true and is exactly why the waste survived a whole port. Parking them: 121%, 4.5 s
    system. A guest thread the title parks needs somewhere in the runtime to park.
98. **When a wait changes from spinning to blocking, every count-based instrument on it
    goes silent.** The `[csspin]` trace fired every 4 M failed attempts, a fine proxy
    for elapsed time while the loop spun and meaningless once it parks — a parked
    waiter reaches 4 M attempts approximately never, so the trace would have gone quiet
    precisely when a wait got long enough to be worth reporting. Re-express the gate in
    a unit that survives the change: elapsed time. And the replacement stats counter
    printed **nothing across six runs** because its report was gated inside a rare
    branch, which read as "no contention" — gotcha 25 in our own tooling for the second
    time in three sessions.

From phase 3 (the window, the present seam and real input):

99. **A window is a thread, and it is *the* thread — so move the guest, not the
    window.** SDL's video subsystem must be initialised, pumped and presented from
    the thread that created the window, and until phase 3 that thread was busy
    running the guest entry point. The guest is the side that moves, because a
    recompiled title's threads are already an abstraction you own (every other guest
    thread here has always run on a spawned `std::thread`) while the windowing
    system's are not. What the main guest thread must keep is its **order** — still
    the first guest thread created, still the first thread id — because that is the
    property a first-occurrence gate can see.
100. **Route the present through the command stream, not around it.** Presenting from
    inside `VdSwap` would have worked and would have been wrong: findings 38-39 were
    entirely about the gap between "the kernel wrote a packet" and "the command
    processor executed it", and a present seam fed from the kernel export keeps
    counting frames straight through a GPU desync. Driving it from `pm4.cpp` case
    `0x64` means a present can only happen at a stream position the parser actually
    reached, and the window's frame count is the same number the ring trace prints
    rather than a second counter that can disagree.
101. **A packet number is a contract, and both obvious implementations are wrong.**
    XInput changes `dwPacketNumber` only when the state changes, and a title may skip
    re-reading the pad when it has not. Ticking it every poll defeats the field's
    purpose; holding it constant while the buttons change hands the guest a press it
    is *correct* to ignore — and that failure presents as "input does not work" when
    it is really "input works and was properly filtered". Move the number in exactly
    one place, on an actual state change.
102. **Between SDL and XInput only two things are conversions, and no filtering is.**
    The buttons are a rename (`SDL_GameController` is the 360 pad generalised). The
    stick **Y axis is inverted** — get it wrong and the game works perfectly except
    that up is down, which reads as a guest bug — and the **triggers are scaled**
    0..32767 to 0..255. A deadzone is NOT the runtime's to apply: the console hands a
    title raw axes and every title has its own, so filtering here invents an input
    characteristic hardware does not have.
103. **A gate that needs a human is a capture request in disguise.** Phase 3's gate is
    "the boot advances on a *real* press", which by construction cannot be
    self-served: there is no synthetic press that is not the arm the gate exists to
    retire. That makes it the same class of work item as a Xenia capture — it has to
    be scheduled with the operator, and everything that does not depend on it should
    be finished and committed first so the only thing waiting is the press.

From the save-data layer (A1 positions 86-92, built straight after phase 3):

104. **When the capture cannot answer, the title's own SDK wrapper can.** Every
    question about a content enumerator is about a RETURN value, and Xenia logs only
    arguments (finding 29). But this title statically links the XDK's own
    `XamEnumerate` (`sub_825D9460`) and its task body (`sub_825D9358`), and reading
    those two functions gave the entire protocol with no guesses: item size 0x138,
    app id at priv+0, message id at priv+4 (which the guest *checks*), the 32-byte
    message, and the exact HRESULT that means "no more". The corollary is the good
    news: whatever the guest only ever hands straight back to you — here priv's whole
    layout — is YOURS to define.
105. **A stubbed query can make a whole subsystem's data invisible with no error
    anywhere.** `XamGetExecutionId` looks like bookkeeping; it is the save
    enumeration's title-id FILTER. An item whose title id does not match is not
    rejected loudly, it is SKIPPED — so an enumeration of nothing but wrong-id items
    is byte-for-byte indistinguishable from an empty one. Both halves failed here at
    once (the stub never wrote its out-parameter, and the enumerator wrote 0 for the
    id). What separated "the title accepted our save" from "the title threw it away"
    was one line printing `XMsgCompleteIORequest`'s arguments — which A1 supplies
    verbatim to compare against.
106. **The save directory is part of a gate's configuration.** A1 was captured with no
    save present. With one, our boot calls `XamGetExecutionId` between positions 90
    and 91 — a call hardware never made because it had no item to filter — and the
    gate reports a divergence. Nothing is wrong in either run. Same shape as gotcha 20
    (`license_mask` defaulting to the trial): run the A1 gate with an EMPTY save root,
    and read a leftover save as a configuration difference, not a regression.

107. **"The capture's next call" is not automatically the next thing to implement.**
    A1's position 93, `KeQueryBasePriorityThread`, has been implemented since phase 1.
    Its only game-side caller is a work-queue drain that runs **only on the failure
    path** (`cmpwi r30,0; bge <skip>`), reached once in an entire hardware boot. So
    matching it means reproducing a failure hardware had, not building a feature.
    Before planning work against a gate position, find its call site and ask what
    CONDITION reaches it — the name says nothing about that.
108. **"Never entered" and "entered constantly with nothing to do" need a probe, not a
    reading.** One `PPC_FUNC` hook on the suspect function and one on each of its
    callers answered in two runs what the call graph could only make plausible: the
    drain is never entered, and three of its seven callers are. `CZ_QUEUE_PROBE` in
    `runtime/cpu/guest_probe.cpp` is the worked example.
109. **A log line that is capped is not a count — and naming the trap does not fix the
    emitter.** `NtCreateFile` successes were printed only for the first 64
    (`n < 64 || FileTrace()`) while failures were printed always, so "the boot opens 64
    files" — quoted in this project since finding 37 — was the cap, not the number.
    This gotcha was written in phase 3 and the emitter was left alone for four more
    sessions, through part 5's tables, because the cap sat EXACTLY at the boot's depth:
    a run printed indices 0..63 and fell silent, so `prologue_z01.big` looked like the
    end of the boot and any change that went further would have been invisible in the
    one column used to score it. Fixed in part 6 (`n < 512`, then every 64th): the boot
    opens **84** files and ends at `#83 skeleton\cinezombie.big`, having passed through
    `cinematics.big` and `700_prologue_intro.big`. Check the emitter before quoting a
    number off a log, the same way gotcha 25 says to check it before believing a zero —
    and when the check finds a cap, RAISE it, because a gotcha in a document does not
    stop the next session quoting the number.

From phase 5 (the renderer; details in `docs/phase5-notes.md`):

110. **A field's width is part of the field, and the symptom lands in another
    subsystem.** A vertex fetch constant's dword1 is `endian:2, size:24, unused:6`, and
    reading the size as `d1 >> 2` with no 24-bit mask turned an 85-dword stream into
    67,108,885 dwords. What that produced was not a wrong picture: it was 2,225,992
    draws reported as "vertex stream outside the physical arena", which reads as an
    addressing or memory-map bug three layers from the actual mistake.
111. **When two tools disagree about an index, the authority is neither of them — it is
    the data structure being indexed.** A vfetch's constant index is
    `const_index * 3 + const_index_sel`, which gives 95/94/93 for the exact shaders
    Xenia's disassembly prints as vf0/vf1/vf2. Reading either tool harder cannot settle
    it. Dumping the POPULATED fetch slots at a draw settles it in one run: the guest
    writes slot 0, the shader that asks for slot 0 by our reading finds it, and Xenia's
    disassembler is displaying `95 - index`.
112. **The frame is a resolve DESTINATION, not the render target.** One title-screen
    frame issues ~20 resolves into the same EDRAM — a 1280x720 main pass, a
    640x360→32x1 downsample pyramid, glyph atlases — and exactly one whose destination
    is the address `VdSwap` named. Presenting the render target instead shows every pass
    overlaid in the top-left at its own size, which looks EXACTLY like a viewport
    scaling bug and is not one: every viewport in the stream is correct. The register
    that supplies the missing identity is `RB_COPY_DEST_BASE`.
113. **Passes communicate through guest memory, so a resolve has to become a
    texture.** Every intermediate resolve here sets `RB_COPY_CONTROL`'s two clear bits
    and the front-buffer one does not — i.e. each pass wipes the EDRAM behind itself.
    A consumer therefore reads its input from the resolve's destination ADDRESS, and a
    renderer that never wrote those pixels anywhere serves it whatever the guest's
    allocator left there. Keying a host image on the destination address and serving
    fetches from it is the whole mechanism; writing the pixels back to guest memory
    would mean tiling them so the consumer could untile them again.
114. **A snapshot must not be cached on the fetch constant.** Its contents change every
    frame while its fetch constant does not, so the ordinary texture cache would freeze
    the first frame's version of that surface forever.
115. **Build the shader cache from YOUR OWN dump, not the emulator's.** The cache key is
    a hash of the microcode, and the emulator dumps it with the emulator's idea of where
    the shader ends. Any disagreement about the length is not a slightly wrong picture —
    it is a total, silent cache miss. Dumping from our own `IM_LOAD` handler makes the
    key agree by construction, AND turns the emulator's dump into a free oracle: 120 of
    our 121 boot-era blobs are byte-identical to A1's, which is the first check this
    port has ever run on that packet's size field.
116. **A capture's blob count is a FILE count.** "455 raw microcode blobs" is 455 files;
    A1's 120 are a strict subset of A2's 335, so there are 335 distinct shaders. Two
    documents quoted the file count as a shader count for a whole phase.
118. **An opcode's FREQUENCY is a statement about the renderer's architecture.**
    `SET_BIN_MASK_LO` has been recorded since phase 1 as this title's most frequent
    type-3 opcode — 2,353,460 of B1's 8,283,322 — and treated as a predication detail
    to get right. It was telling us the thing that mattered most: the Xbox 360's EDRAM
    is 10 MB and a 1280x720 colour+depth target does not fit, so Case Zero renders its
    scene in **two 640-wide tiles** into a 640-pitch EDRAM surface and resolves each
    into its half of one 1280x720 destination. 930 draws and 494,667 vertices a frame
    were being rendered and thrown away for want of that fact.
119. **A copy's SURFACE and its REGION are different registers.** `RB_COPY_DEST_PITCH`
    is the destination surface; `PA_SC_WINDOW_SCISSOR` is the part of it this pass
    covers, in screen coordinates. Copying the surface's extent when the region is a
    tile puts every pass's content in the top-left corner at assorted sizes — which
    reads as a viewport scaling bug, and survived one whole correct-but-incomplete
    diagnosis (gotcha 112) before the draw-count-per-pass trace exposed it.
120. **Count the draws PER PASS, not per frame.** It is the number that separates "this
    pass rendered nothing because it had no draws" from "this pass had 930 draws and
    produced black" — two completely different investigations that look identical in a
    snapshot. Adding it reframed the entire hunt in one run.
121. **Tiles of one surface must share a key.** The second tile's `RB_COPY_DEST_BASE` is
    pre-offset into the SAME allocation: `06BF8000 - 06BE4000 = 0x14000`, exactly the 20
    macro-tiles that 640 pixels of a 4-byte tiled surface occupy. Keyed on the raw base,
    one surface looks like two and a consumer fetching its real base gets only the left
    half.
122. **`numFormat=integer` is not a shader-side detail.** A normalized vertex format
    divides by the type's range, so an integer 32 arrives as 0.125 and a shader that
    `floor()`s it to index something reads element 0 every time. Vulkan's
    USCALED/SSCALED are exactly the missing concept — an integer delivered as its own
    value into a FLOAT input, which a `*_UINT` format would not be.
123. **Size a constant buffer from the register the guest writes, not from a tool's
    documentation.** XenosRecomp's README says the pixel-shader constant window is 224
    float4; the shaders it GENERATES read up to `c255`. Our 224-register buffer meant
    `c255` loaded 512 bytes past its end. Case Zero's scene shaders use c255 as the
    tone map's scale and bias in their FINAL instructions, so a wrong c255 does not
    tint the scene — it collapses every pixel to a constant. The guest says the true
    size in `SQ_PS_CONST`: base 256, size 255, i.e. 256 registers.
124. **A per-(vs, ps) draw census plus the capture's disassembly is the pair that
    localises a shading bug.** The census says which shader does the pass's work; the
    disassembly beside every blob in the capture says what that shader was supposed to
    compute. Neither is useful alone, and together they took "the scene is flat" to a
    named constant register in one run.
125. **`g_SwappedTexcoords` corrects something the RUNTIME does, not the guest.**
    Dword-swapping a vertex stream is right for 32-bit components and transposes the
    halves of every 16-bit pair, so a `16_16` attribute arrives YX. The shaders un-swap
    it when the runtime sets the matching bit — and a mask left at zero silently swaps
    the components of every 16-bit vertex attribute in the title.
126. **An experiment consistent with BOTH hypotheses has tested neither.** The first
    fetch-slot probe (gotcha 111) caught a shader that asked for slot 0 twice while
    both slot 0 and slot 95 were populated — so it was consistent with either reading,
    and it still produced a confident answer. The unambiguous version is an arm that
    inverts the convention and looks at the result: inverted, the scene renders 0.0%.
    The conclusion was right; the evidence for it was not what it appeared to be.
127. **A metric taken from an ANIMATED scene is not a metric.** Case Zero's title
    screen renders a live 3D background (capture E says so outright), so "the scene
    surface is N% non-black at frame 600" is a different camera angle every run. A
    single-run A/B on it produced a confident "this change made it worse" that
    alternating 3-against-3 showed was pure noise — 100.0/64.1/97.5 against
    64.4/94.8/79.6. Gotchas 50, 51 and 86 in a new place, and quoting them all over
    this project did not prevent it. Large structural changes (0% -> 70%, 3 colours ->
    848) are still readable; anything smaller needs a pinned camera or B1's own
    per-era aggregates.
128. **A hand-patched shader cache is a five-minute experiment.** `CZ_SHADER_SPV=<dir>`
    points the runtime at any cache, so "is this generated line doing what it looks
    like?" can be answered by rebuilding one shader with the line changed and running
    the same binary. It retired the `sges` idiom (`abs(x) >= 0.0`, the compiler's
    "set w = 1") as the cause of exploded geometry: forcing it to a literal 1.0 changed
    the picture by 0.1%.
129. **A metric that passes perfectly and tests nothing is worse than a noisy one.**
    Aligning rendered frames by content — the method that works on the capture pair —
    gave 257 of 257 frames bit-identical with zero delta, and it was 257 copies of a
    BLACK IMAGE. For a scene animated off wall-clock time, exact alignment selects for
    stasis, which is precisely the content least able to reveal a difference. The same
    100.0% came back for arms that visibly change the picture. The noisy version at
    least did not invite belief.
130. **Aggregate over the era; and use the MEDIAN, not the mean.** Several hundred
    frames sample the whole animation cycle, so the median is stable (1.36 pp over five
    runs of one binary) while the mean is not (64.3–70.8 over the same runs) — the mean
    is pulled about by how long a run spent in each part of the cycle.
131. **Measure the surface the defect lives on, not the one you present.** The first
    version of the metric compared the presented frame, which at this title screen is
    mostly UI: a change touching 476,858 draws a run moved it 0.1 pp. It could not see
    the defect it was built for.
132. **A threshold derived from the runs being compared cannot fail.** It widens to
    accommodate whatever difference is present. `frame_compare.py` quotes its 1.5 pp
    band as a constant measured once from five baseline runs.
139. **A probe that fires on the first occurrence samples the BOOT.** `CZ_VK_DRAW_PROBE`
    reported "every pixel-shader constant is zero" for a post-processing shader whose
    constants are a perfect Gaussian kernel by the title screen — because a shader's
    first three draws happen before the guest has uploaded the constants it will use.
    Watching the register itself settled it in one run (no zero writes at all after
    frame 400). Bound every state probe by FRAME as well as by count; this is the same
    trap as bounding it by vertex count and finding only the small early meshes.
From phase C part 2 (the movie deadlock, and how it was finally named):

141. **When you interpose on a stream, the question for each packet is not "can I
    emulate this?" but "WHO READS IT?".** Redirected emission is right for content,
    whose only consumer is your renderer, and wrong for anything the title itself
    reads back. Case Zero's callback hand-off is an arming of two GPU scratch
    registers, three `WAIT_REG_MEM`s that hold the GPU until that mirror is visible
    in memory, an `INTERRUPT`, and a re-poison — and the ISR reads the callback out
    of GUEST MEMORY at delivery time, so the whole thing is an ORDERING contract
    against the CP and against the guest's own stores. Four increasingly clever
    emulations of it all raced the poison. Emitting the block into the stream whose
    reader owns it took two dozen lines and no semantics at all.
142. **A probe is worth writing when it can run on BOTH arms — that is what makes it
    an oracle rather than a description.** Three sessions of hypotheses about
    interrupt races ended with one flag (`CZ_FENCE_PROBE`) hooking the five functions
    that produce the fence/callback protocol, run once on the PM4 control arm and
    once on the D3D arm, and diffed: the control arm's ISR delivered `sub_8284AAD0`
    138 times and ours delivered it ONCE. The missing call had never even been named
    before that line printed. Instrument the seam both arms share, not the arm you
    suspect.
143. **A "no callers / no writers" scan result is a statement about STATIC form, not
    about behaviour, and one hardware watchpoint settles it.** `dev+0x2B04` is
    incremented by a `stw` a scan finds instantly and decremented by nothing the whole
    8.8 MB image contains — because the decrementer holds the pointer in a register.
    `gdb -p`, `watch *(unsigned int*)((char*)g_memory.base + <va>)`, two continues,
    two backtraces: `sub_8284A960` under the worker's token interpreter, in one run.
    (Read the values as big-endian: guest 2 prints as 33554432.) The same trick names
    the caller of an indirect-only function.
144. **Name a function by what its ARGUMENT does, not by its shape.** `sub_82846288`
    was recorded for a phase as "fence/throttle-shaped" on the strength of its call
    rate and its comparison of two cursor fields. It is the callback armer, and the
    thing that said so was `tools/gdis.py --find-uses` on the ADDRESS of the callback
    it arms — the constant `0x8284AAD0` appears at exactly three call sites, all
    passing it as an argument to this function. A 32-bit constant is never one
    instruction, so grepping the image for it misses every one.

145. **A spin on a counter is a claim about a VALUE, so print the value.** Two
    sessions described `sub_82846210` as "waiting for the async segment count to reach
    zero" and planned work around who was failing to decrement it. The word held
    **-552**, and the loop tests `!= 0`: it was not slow, it was unsatisfiable, and the
    real imbalance ran the other way (6 increments against 18,900 decrements). One
    `%d` in a probe line reframed the whole hunt. The corollary is that a counter
    described as "never returns to 0" deserves the question "in which direction?"
    before anything is built on it.
146. **A fence word pinned to a CONSTANT while its emitted counter climbs is replay,
    not latency.** `[wb+0]` froze at `0x00000795` at exactly the frame the ring went
    runaway, and the obvious reading — the GPU has fallen behind — is wrong in a way
    that sends you to the renderer. A stream being re-executed keeps rewriting the same
    stale `EVENT_WRITE` value, so the word does not lag, it repeats. Lagging and
    repeating look identical in a single sample and completely different in two.
147. **Redirected emission moves BOUNDARIES, not just packets — and a boundary has a
    reader too.** Part 2's rule ("emit where the reader lives") was stated about
    packets. A segment's extent is `[dev+0x3B20] .. [dev+0x30]+4`, and the content that
    would normally separate a hand-off block from the next segment boundary is exactly
    what phase C redirects away. So the block can end up inside the segment its own
    wake-up resubmits, which is a loop with gain one and no seed.
148. **A retired hypothesis is retired against a binary, and moving code can hand it
    back its premise.** `CZ_PM4_STOP_ON_WAIT=1` was measured and retired for the arm
    block's trailing `WAIT_REG_MEM` — while that block was in the private scratch,
    where the walker's own wait handler never stalls and the flag *could not apply to
    it*. The measurement was honest and the conclusion was about nothing. Re-run it the
    moment the thing it gates moves. Gotcha 13's shelf life, applied to our own notes.
149. **The cheapest way to ask "who reads this?" is to label every address you print.**
    Phase C's whole difficulty is that one dword can live in the ring (read by the
    title) or in our private scratch (read only by us), and a bare `%08X` cannot tell
    you which. Tagging every cursor in the probe with ` SCRATCH` turned three separate
    mysteries into one glance — and it is what found that 405 of 405 callback armings
    were in the wrong stream, and that the frame-end submit was reasoning about a
    buffer the title never sees.

From phase C part 4 (the replay is the flywheel, not the fault):

150. **A feedback loop with gain one is a PIPELINE while its pointer advances and a
    runaway the moment it stops — so "this loop regenerates its own wake-up" is not
    by itself a defect.** Case Zero's GPU→ISR→worker→ring hand-off does exactly that
    on hardware and on our PM4 control arm: each frame's arm-carrying segments produce
    the interrupts that drive the NEXT frame's walks, because the guest arms with a new
    token buffer every frame. Three sessions read the regeneration as the bug. It is
    the design; what breaks is that the phase C arm's guest stalls, the pointer stops
    moving, and the same buffer is walked forever. Before theorising about a loop, ask
    whether its state advances — and the cheapest form of that question is to print the
    ITERATION'S IDENTITY (here the token-buffer pointer) and flag repeats.
151. **A conditional inside an instrument can make every measurement of it a
    measurement of nothing.** `CZ_PM4_STOP_ON_WAIT` was gated on `depth == 0` — the
    ring itself — and every wait it was aimed at lives inside an INDIRECT BUFFER, i.e.
    at depth ≥ 1. It was measured and retired TWICE, once on the grounds that the
    packets were in the wrong stream and once on the grounds that they had moved to the
    right one, and on both occasions the flag could not fire. Gotcha 148 says a retired
    hypothesis is retired against a binary; this is the sharper form — read the arm's
    own code before quoting what its absence proved. Two symptoms to recognise: an arm
    whose "on" run is byte-identical to its "off" run, and an arm with no counter
    saying how many times it engaged.
152. **A stall below the top of a nested walk needs a resume plan, or the retry is a
    replay.** Unwinding an indirect buffer and re-entering it next tick re-executes
    every packet before the stall — including the arm and the INTERRUPT the stall
    exists to hold back — so the fix makes the symptom. Record (buffer, dword) per
    depth and resume there; and do not count a deliberate stop as a truncation, or the
    one live alarm finding 39 left behind sits permanently nonzero.

From phase C part 5 (the missing CPU side of the hand-off was a display controller):

153. **A register the title only ever READS cannot be found by hunting for its
    writer.** Every previous hunt in this project started from a store — the ring kick,
    the counter's decrementer, the callback armer — and `CZ_PM4_MEM_WATCH` is a
    store-side instrument by construction, so it answered "who writes this word" with
    3,089 writes of the value 1 and complete silence about the zero. The zero came from
    the CPU, behind an MMIO **load** at `0x7FC86544` that appears exactly once in 8.8 MB.
    What found it was scanning for the aperture BASE (`lis rX,0x7FC8`) and enumerating
    every load and store built on it: this title's entire GPU MMIO surface is **five
    instructions**, four registers, and only one of them is read.
154. **One hardware status bit can gate an entire subsystem, and its absence looks
    exactly like nothing at all.** Behind that bit is the guest's own swap queue —
    16 records of `{surface, due tick}`, a vblank tick, a walker — and with the bit
    clear the walker has no caller, the tick stays 0 for the life of the process, and
    the queue grows to 1,540 entries with **one** ever retired. No error, no log line,
    no wait that visibly fails; the only symptom is a GPU wait somewhere else that
    never completes. Fable 2 hit the identical gate at the identical address, which is
    the real lesson: when a hand-off has a CPU side you cannot find, read what the
    previous port did about the display controller before theorising about packets.
155. **An interrupt can be addressed to a SET of hardware threads, and a
    single-threaded ISR acknowledges exactly one of them.** Case Zero's arm block
    writes a six-bit CPU mask; the ISR clears `1 << PCR[0x10C]`; the block's trailing
    `WAIT_REG_MEM` holds the command processor until the whole word is zero. The mask
    DEFAULTS to CPU 2, which is where our pump has run since phase 1 — so the common
    case was right by accident and the one caller that names CPU 4 deadlocked the ring.
    A runtime with one ISR thread has to take the interrupt once per named CPU,
    reporting each in its PCR; anything else is one acknowledgement short. And the ISR
    body is per-CPU too (the D3D job ring is at `dev + cpu*0x6C + 0x2C40`), so the CPU
    number decides *which worker* gets the kick, not just who acknowledges.
156. **A brake and its release are one mechanism; half of it is worse than neither.**
    The CP stalling at `WAIT_REG_MEM` and the vblank handler clearing the word it polls
    are this title's entire frame pacing. With only the release, the title free-runs and
    its flip queue overflows (head 27, tail 1,074). With only the brake, the ring parks
    at frame 1. With both, head equals tail, one record retires per frame, and the boot
    reaches the title screen with `truncated=0`. Two changes, each of which measures as
    a regression on its own.

From phase C part 6 (the brake promoted, and the harness that nearly decided it wrong):

157. **A metric whose discriminator differs between the arms cannot compare them.** The
    brake's health was first measured as "what fraction of stalls are RELEASED", and a
    release was detected by the stall's address changing. That reads the two arms
    differently for a reason that has nothing to do with the title: phase C re-emits its
    hand-off block at a FIXED private-scratch address every frame, so a healthy re-stall
    looks identical to being stuck, while the PM4 arm's blocks rotate through ring
    addresses so every re-stall looks like a release. Same behaviour, 100% against 4.9%,
    and it produced a confident "the draw arm's ring is chronically parked" that had to
    be retracted in the same session — the swap queue was retiring one record per frame
    throughout. The fix is a quantity with no such dependency: **consecutive ticks spent
    on ONE wait**, which reads 1 / 2 / 5,491 for paced-control / paced-draw / parked.
    Before comparing two arms, ask whether the METRIC means the same thing on both.
158. **Build the negative control before believing the instrument, not after.** Two
    successive versions of that counter were wrong, and neither was caught by reading
    the code. What caught both was running the configuration whose answer was already
    known — `CZ_ISR_SINGLE_CPU=1` with the brake on, which part 5 measured parking at
    frame 7 — and seeing it report `released=4568` for a run that managed 6 frames. A
    counter that has only ever been pointed at healthy runs has not been shown capable
    of reporting sickness (gotcha 30, applied to instruments rather than tests).
159. **A bimodal arm makes every single-run claim a coin flip.** The phase C draw arm's
    default configuration produces 332 to 3,451,841 frames in the same 120 s — three
    near-stalled runs and seven runaway out of ten. Part 5's "1,745" and "2,856,448" are
    two modes of one distribution reported as two measurements. Gotchas 50-51 say to
    measure a rate; this is the sharper form — a MEDIAN over runs is not enough either,
    because a bimodal median lands between two states the system never occupies. Print
    the runs.
160. **A long campaign leaves the gates a free control — use it on the one number that
    is not flat.** A1's position-71 window permuted on the promoted binary, which reads
    as a regression. Re-gating the 20 saved control-arm logs cost nothing and gave
    4 of 10 with the brake on against 1 of 10 with it off: not distinguishable at that
    sample size (Fisher p ~= 0.30), no cost in boot depth or in A5. Gotcha 95 again, and
    the discipline is to report the rate INCLUDING when it is mildly unfavourable rather
    than quote the one clean run.

From phase C part 7 (the ~300x amplification, retired — it was never a ratio):

161. **A ratio between a counter that is still running and one that has stopped is a
    STOPWATCH, not a gain — and it reads like a mechanism.** "One guest arming produces
    ~300 ISR deliveries, unchanged by the brake" survived a whole session as the port's
    top open question. The denominator freezes seven seconds into the boot and the
    numerator does not, so the same binary reads 1.8x at 8 s, 10x at 35 s and 30x at
    78 s. Nothing multiplies anywhere: measured link by link the chain is
    `ints/arms` = 0.9997, `isr/ints` = 1.000, `walks == kicks == drains`. Before
    believing any X-per-Y, plot BOTH series against time and check they are both still
    moving; a frozen denominator is the finding, not the ratio built on it. It caught
    me twice in one session — the second time on "6 increments against 1,873", which is
    1.0 per frame against 3.0 per frame once the stopped run's frames are counted.
162. **Count a chain LINK BY LINK, in one always-on line, or you are comparing two
    printers.** The old figures came from a line-budgeted probe, two hooks, two greps —
    so neither half was a count (gotcha 109) and their ratio was meaningless twice over.
    Unconditional relaxed atomics on hooks that already exist cost nothing, appear on
    every run including the ones saved for something else, and turn "which step
    amplifies" from an argument into a column. `cpu/chain_stats.h` is the worked
    example; the load-bearing field is `distinct`, the number of DIFFERENT values the
    loop has iterated on, because that is what separates a pipeline from a replay.
163. **A fence word that REGRESSES is proof; a fence word that repeats is only a
    suspicion.** Gotcha 146 recorded that a completion word pinned to a constant means
    replay rather than latency. The sharper form was already sitting in an existing
    trace: two consecutive prints of the engine's own wait showed `completed=1023` then
    `completed=1017`, and one memory watch showed the word on a **nine-value carousel**,
    440 laps in 4,000 stores. A wait for anything past the top of the carousel is not
    slow, it is unsatisfiable — and no single sample of that word can say so.
165. **The most visible symptom is rarely the blocker, and an arm that ENGAGES and
    changes nothing is the cheapest way to find that out.** The draw arm's fence word
    demonstrably regressed, so "stop it regressing" looked like the cure and was one
    flag away. It refused 5,711 backwards stores in 90 s — no dead-arm doubt, unlike
    gotcha 151 — and the boot froze in the identical place. That negative result is
    worth more than the fix would have been: it says the wait is for a fence beyond the
    top of the carousel and the packets carrying it are never EXECUTED, which is a
    different fault in a different subsystem from the one the symptom pointed at.
164. **A stall's ERA names the feature that starts there, and the file it stops on is a
    coincidence of timing.** "The draw arm stops at #60 `models\zombies.big`" has been
    quoted as a loading depth for four sessions. It is the frame at which the title
    first renders its scene as two 640-wide tiles (gotcha 118), which is the only place
    in the image that arms the worker callback and queues its own segment to the worker
    in the same breath. Ask what the title STARTS DOING at a stall, not what it was
    reading.

From phase C part 9 (the picture — four defects between the scene and the screen):

171. **A counter behind an early return counts the times the early return did not
     happen, and its silence is unfalsifiable from inside.** `texture: resolve snapshot
     too old` read **7** on the binary whose texture cache was consulted BEFORE the
     snapshot check, and **70,681** on the fixed one — because on the broken binary the
     cache hit short-circuited before the snapshot was ever looked at. Every other
     instrument reported a healthy chain while the whole scene composed black: the LUT's
     own resolve snapshot was 99.9% non-black, the tone map's four other inputs were live
     snapshots, its colour mask was F, its constants were sane, and `texture: cache hit`
     read 2.2 M and looked like health.
172. **A retirement is only as good as the ORACLE it was measured on.** Index endianness
     was A/B'd and retired in phase 5 ("the packet's own code beats both overrides by two
     orders of magnitude"). The A/B was honest; it was scored on a frame that was black
     for an unrelated upstream reason, so all three arms measured the same nothing. When
     the upstream defect was fixed, the same arm turned exploded geometry into Still
     Creek. Gotcha 13's shelf life, pointed at your own earlier measurements.
173. **"Never submitted" and "submitted and rejected by stale depth" are the same
     picture and completely different investigations.** One arm separates them —
     `CZ_VK_NO_DEPTH_TEST=1` took the scene tile's coverage from columns 0..320 to
     0..640 and thereby proved the draws had been there all along, which moved the hunt
     from the geometry to the CLEAR. Build that arm before theorising about missing
     geometry; it is four lines.
174. **A screen-space rectangle reaches the screen through two conventions, and each
     one can lose half of it.** A Xenos rectangle list stores three corners (TL, TR, BR)
     and the hardware generates `v0 + v2 - v1`; an index rewrite cannot name a vertex
     that does not exist, so "reuse the three real corners" covers exactly half of every
     rect — and these draws are the guest's per-pass CLEAR, half of them depth-only, so
     the other half keeps the previous pass's depth and rejects everything behind it.
     Then window coordinates are in PIXELS while an EDRAM image is at SAMPLE resolution:
     on a 4x-MSAA surface (which doubles the width on Xenos) a 320-wide clear IS the
     640-wide tile. Both present as a picture with a diagonal edge, i.e. as broken
     geometry.
175. **A packet's field can be in a different dword from every other packet's, and the
     packet states the count twice so you can check.** DRAW_INDX carries its index
     buffer's endian swizzle in the TOP two bits of the SIZE dword, not the low two of
     the ADDRESS — and reading it off the address ALSO masks away address bit 1, which
     is real for a 2-byte-aligned 16-bit index buffer (~40% of this title's draws). The
     symptom is triangles radiating from the exact screen centre, which is NDC (0,0) and
     therefore says "the indices are garbage", not "the transform is wrong". `init >> 16`
     and `size & 0xFFFFFF` agree on every draw of a boot; that agreement is now a
     standing check.
176. **A title screen can be TWO screens.** Case Zero's alternates a 49-frame logo pulse
     (capture E2) with a long animated 3D background era (capture E3), so a frame dump
     every 64 frames is a Bernoulli trial over which one you are looking at — and three
     phases of "the title screen is black" were single samples of the era that was
     broken while the other one was already correct. Measure ALL the dumped frames, not
     one; the two eras differ by 2.31% vs 37% coverage and 880 vs 81,014 colours, so one
     `awk` over the stats file separates them.

177. **`git add -A` commits what is in the tree, not what you changed — so diff the
     whole subsystem against the last known good commit before committing.** Five files
     were rolled back to their pre-part-8 content by something outside the session's
     edits, and the commit took phase C part 8's entire fix out with it; the draw arm
     regressed to `ints/arms` in the tens and stopped at `#60` again. The tell was in
     `git diff <good> HEAD --stat -- runtime/`: deletions in files the session never
     opened. And the thing that proved it was not the session's own work was reverting
     every one of its changes THROUGH THEIR ARMS and watching the failure survive — an
     arm's job is as much to exonerate as to convict. Corollary: a session that only
     runs one arm cannot see a regression in the other, and this one nearly shipped it.

From phase C part 10 (the right tile, and the oracle that was sitting in the capture):

178. **When your own subsystem is the suspect, it cannot be its own oracle — but the
     capture can replay YOUR RULE.** Three phases treated "a third of this title's draw
     packets are discarded by bin predication" as a fact to explain. Replaying the same
     `(header & 1) && (mask & select) == 0` test over B1 took an hour to write and said
     hardware discards **0.3%** against our 33%, with both tiles offered exactly 575,744
     draws and each keeping 99.5%. That killed four ranked hypotheses at once, and one
     part later the cause (gotcha 185) brought us to 0.28%. The
     enabling detail is general: Xenia writes a `PacketStart` for every packet BEFORE it
     evaluates predication, so the capture contains the packets hardware skipped, and any
     rule your command processor applies can be replayed against it.
179. ~~**A value written as a LITERAL and patched later is indistinguishable from a
     computed one until you look for its writer.**~~ **RETRACTED by part 11.**
     `0x80000000` was not a placeholder: it is a deliberate trailing reset, the
     placeholder is the LEADING `SET_BIN_MASK_LO FFFFFFFF`, and the fix-up pass had NOT
     "run once and patched nothing" — that was a probe printing only its first call
     (gotcha 186). It ran 1,751 times over 388,451 records and computed "touches no
     tile", because the screen extent it intersects was never written (gotcha 185). The
     surviving half of the rule is still worth having, and it is the question that
     actually cracked this: "is there any code that computes this KIND of value at all",
     then INSTRUMENT that code rather than reading it.
180. **Trace the WRITES, not just the reads, and hold the budget until the era that
     matters.** A draw-only bin trace can only restate the symptom. The write trace made
     the two streams comparable line by line — and the first 300,000 packets of both are
     packet-IDENTICAL, so a trace armed at the start would have shown perfect agreement
     and proved nothing. `CZ_PM4_BIN_TRACE_ARMMASK` exists because gotcha 139's rule
     (bound a probe by the era, not by the count) applies to stream traces too.
181. **An arm nobody has re-measured is where last session's fixes are hiding.** Part 9
     fixed four renderer defects on the PM4 control arm and could not re-gate the phase C
     draw arm. Re-gated with zero new changes, that arm went from `#60` and
     `arms:ints = 12:856` to `#83`, `0.9998`, `distinct=816-911`, engine counter 0, and
     an exact 84-prefix on A1 — 6 of 6 serial runs, the healthy shape part 7 defined and
     the port had never reached. Gotcha 67 says an implemented import that never ran is a
     prediction; so is a fix that has only been measured on one arm.
183. **A second copy of your own binary is not background load — it is an INTERVENTION on
     the variable under test.** Two overlapping run loops put two 170 s draw-arm boots on
     the machine at once, and one of them reported `#60`, `arms=241 ints=207,599`,
     `distinct=6`: part 7's stall, exactly. That reading was written up as "the arm is
     still bimodal" and was wrong — the clean serial set is 6 of 6 healthy with a 1.09x
     frame spread. When the quantity being measured is decided by multi-threaded
     scheduling, halving the effective CPU changes it. **Run timed arms serially.** The
     useful half: contention is now the cheapest known REPRODUCER for that stall, which
     otherwise has to be waited for.
182. **A "delete this dead code" recommendation expires when the code's regime
     changes.** The walker's INTERRUPT block and `MirrorIsPoisoned()` recorded zero on
     every arm — measured on a draw arm that stalled at `#60`. The arm now runs to `#83`
     with a completely different chain shape, so those zeros describe a machine that no
     longer exists, and both are guards against a crash that was real. Gotcha 13's shelf
     life, pointed at a deletion rather than a finding.

From phase C part 11 (the screen extent — a packet implemented and never executed):

184. **A packet you IMPLEMENT and a packet you implement for every FORM it takes are
     different claims, and the census that separates them is keyed on the packet's own
     fields.** `EVENT_WRITE_EXT` has had a name in this port's opcode table since phase
     1, appears in every census, and passed both capture oracles — and did nothing
     818,507 times a boot, because the fence family's handler stores only when a packet
     carries a value dword and this form carries an address and none. Census a capture
     by `(opcode, body length, event type)`, not by opcode, and read any row your
     handler falls through as a hole. Gotcha 88's other half: the oracles check your
     ARITHMETIC, and ours was right.
185. **A GPU can be an INPUT to guest logic, not only an output.** Xenos's screen-extent
     query (`EVENT_WRITE_EXT` event `0x1A`, paired with `EVENT_WRITE 0x19`) writes the
     rectangle the GPU just rasterized into guest memory, and this title feeds it
     straight back into next frame's bin masks. A recompiled runtime that renders on the
     host and never writes those extents leaves the guest intersecting UNINITIALISED
     MEMORY against its tile rects — 76% of records came back "touches no tile" — and
     the symptom lands four layers away, as a scene tile that renders nothing. Where a
     query cannot be answered honestly, answer it CONSERVATIVELY: an extent larger than
     any tile makes predication a no-op, and too large only costs work while too small
     silently deletes geometry.
186. **A probe that reports "1" is reporting its schedule until you have read the
     schedule.** Two probes here printed at call #1 and then every 20,000, against
     subsystems that run a few thousand times a boot — so every run emitted exactly one
     line, whose counts are all 1 by construction, and a whole session's conclusions
     ("the pass runs once, patches zero records, behind a closed gate") were built on
     it. Report on a CLOCK, not a call count: a time-based schedule cannot be wrong
     about a rate it has never seen. Gotcha 109 in our own instruments, twice in three
     sessions.
187. **The capture can replay your stream-order window, not just your rule.**
     `xtr_bin_predication.py --trace-window N --trace-arm-mask HEX` prints the same
     window `CZ_PM4_BIN_TRACE` prints for us. Hardware's read `MASK_LO 8000000F ; DRAW
     -> run ; MASK_LO 80000000 ; MASK_LO 8000000F ; DRAW -> run` and ours read `MASK_LO
     80000000 ; MASK_LO 80000000 ; DRAW -> SKIP`. Four lines said "the leading mask was
     computed and came out empty" where a whole phase had read "the mask is an unpatched
     placeholder". Build the capture-side twin of every stream instrument you own.

188. **A defect one column wide is invisible to every aggregate and obvious to a human,
     because the frame's own blur AMPLIFIES it.** The half-pixel offset shifted every
     vertex by -0.5 px, so a screen-space rect `[0, W]` became `[-0.5, W-0.5)` and its
     last pixel centre landed exactly on the exclusive right edge, where the fill rule
     drops it — the scene tile's clear covered 0..638 and column 639 was covered by
     NOTHING. That is 0.08% of the frame, far under `frame_compare.py`'s 1.5 pp band;
     convolved with the scene's depth-of-field kernel it becomes a 19 px dark band, and
     the operator saw it in one glance. Gotcha 135's mirror image, and the same
     conclusion: an aggregate over pixels cannot see a single-column defect, so the
     metric has to be STRUCTURAL and within-surface. `all-black columns in the resolved
     surface` reads 1 -> 0, is deterministic, and needs no era alignment.
189. **The pixel-centre convention needs no shift for COVERAGE, and adding one costs a
     column.** Xenos samples centres at integers, Vulkan at half-integers; a rect
     `[0, W]` covers W pixels under BOTH. The classic D3D9 half-pixel fix is about
     texel-to-pixel alignment, not coverage, and applying it to geometry is a
     subpixel shift of everything. Before inheriting one, ask which of the two it is.
190. **A screen you cannot reach without a human is a screen nobody can measure.**
     `CZ_FAKE_START_MS` presses only START, so every defect more than one menu level
     past the title was an operator report with no headless reproduction —
     `CZ_FAKE_PRESS_SEQ=START,A,A` is the fix, and the design detail that matters is
     that it HOLDS its last entry rather than wrapping (a wrap walks back out of the
     screen it was aimed at, and the run oscillates between two menus with no way to
     tell that from a frame dump). Gotcha 103 said a gate needing a human is a capture
     request; this is the other half — extend the arm until it is not.

191. **A range that is not the range answers a different question, confidently.** "39
     textures uploaded black and their guest memory reads NON-ZERO now" was the whole
     case for "we cached a texture before the guest streamed its pixels in" — and it was
     measured over the 8 KB tiled FOOTPRINT of textures whose own texels are 128 bytes,
     so it was mostly reporting on the NEIGHBOURING textures. Asked of the bytes the
     untile actually reads, the answer is **zero of 58**. Two ways to catch it: make the
     predicate and its re-check read the identical bytes by construction (ours did not,
     so entries flip-flopped forever), and be suspicious of a footprint that is 64x the
     object inside it.
192. **A repair that engages hard is not a repair that works, and a metric can rise
     while the picture collapses.** The one built on that number fired 3,258 times a
     boot — no dead-arm doubt (gotcha 151) — and turned the save-slot panel entirely
     WHITE, because each re-upload took a fresh bindless slot and exhausted the
     4096-entry heap: 62,619 fetches served the 1x1 dummy. A recycled resource is part of
     any invalidate-and-redo path, and the symptom of not recycling it is not a leak, it
     is every later texture silently becoming the fallback.
193. **A documented instrument can not exist.** `CZ_VK_PASS_DRAWS=N` sat in this file's
     instrument list for three phases with a stated default; the count was a hardcoded
     literal and the environment variable was read nowhere in the tree. Gotcha 25 says to
     grep the emitter before believing a zero — the same applies before quoting a knob,
     and the check is `grep -n CZ_VK_PASS_DRAWS runtime/`.
194. **Two arms of one runtime can be mutually exclusive, and setting both silently
     gives you the wrong one.** `CZ_D3D_DRAW=1 CZ_VKDRAW=1` is the PM4 arm: the runtime
     disables the draw arm and says so once, in a line that scrolls past. Three "draw
     arm" runs here were PM4 runs, and they agreed with each other AND with a control
     perfectly — consistency is no defence, because both sides of the comparison were the
     same wrong thing. The tell was in the chain counters (`arms=74, kicks=0, walks=0`
     against the real arm's `arms=12627, kicks=6752`). Confirm an arm ANNOUNCED itself
     before reading anything else off its log.

From phase C part 13 (the UI's text layer, and a crash that was an assertion):

195. **A draw packet has no base-vertex field, so a title that sub-allocates ONE vertex
     buffer between draws can only do it with `VGT_INDX_OFFSET` — and ignoring that
     register drops nothing and errors nowhere.** Case Zero's whole UI fills one dynamic
     buffer per frame and issues 115 draws whose fetch constant never changes address;
     only the offset moves, by exactly the previous draw's index count (0, 16, 20, 68,
     84, 88, 136, ...). Every draw therefore rendered the FIRST run's vertices, so
     exactly one text run came out correct and every other one was that same run's
     glyphs sampled through whichever atlas it bound. Two sessions read that as a
     property of the two ATLASES, which is the visible difference between the draws and
     the cause of neither. `gpu/xenos.h` had the register and `CZ_VK_STATE_PROBE` had
     been printing it since phase 5; all three submission paths passed 0.
196. **A probe that prints one component of a vector has not printed the vector.** The
     draw probe showed a single dword per vertex for non-position attributes, which for
     a `32_32_FLOAT` texture coordinate is `u` and not `v` — so two draws sampling
     different atlases produced transcripts that agreed on every printed value and
     disagreed on the one that mattered. The bit-identical `u` was the finding, read as
     a coincidence. And one quad is one GLYPH: bound a probe's vertex count by what the
     structure under investigation repeats at, not by what fits on a line.
197. **In a recompiled image a guest ASSERT presents as a null-pointer crash.**
     XenonRecomp lowers `twi`/`twui` — PowerPC's unconditional trap, and how a `dbAssert`
     stops — to nothing, so the deliberate `stw rX,0(0)` that follows it is what faults,
     and the crash reporter truthfully reports guest address 0. The tell is a `twi`
     immediately above the faulting store; the assertion's own text is two `lis`/`addi`
     pairs away (gotcha 144). Case Zero's said
     `0 && "Bad file digest.  Please re-link the executable and try again."` from
     `digestmanager.cpp`, which named the entire hunt in one string.
198. **Gotcha 57 applies to BREAKPOINTS, not just to crash dumps.** `ctx.rN` read under
     `gdb` in the middle of a recompiled function is stale — the compiler keeps
     `PPCContext` fields in host registers — so two attempts to read a computed digest
     off the guest stack returned twenty zero bytes and a completely different code
     path's registers. At a function's ENTRY the values are fresh, which is exactly what
     a `PPC_FUNC` hook gets for free. Hook the function; do not breakpoint its middle.
199. **A hardware watchpoint on one alias cannot see a write through another.** The
     physical arena is mapped at `0xA0000000`, `0xC0000000` and `0xE0000000` from one
     memfd on purpose, because the guest converts pointers between the views by
     arithmetic. Watch all three or the silence means nothing. (And the runtime prints
     `runtime: guest memory at 0x...` unconditionally, so `gdb` needs no symbol lookup
     to turn a guest address into a host one — `g_memory` does not resolve as a minimal
     symbol from every frame.)
200. **A skip list keyed on NAMES cannot state the condition it stands in for.**
     `main.cpp` skipped `.reloc / .XBLD / .edata / .idata` under the comment "not read at
     runtime and, in this XEX, their source ranges over-run the loaded image buffer".
     Exactly one of the four over-runs. `.idata` is where this XEX keeps its RESOURCES,
     so skipping it made `XexGetModuleSection("Digest")` twenty-eight zero bytes and cost
     four sessions. Write the check, not the list: a check is re-evaluated every build
     and a list is inherited without its test.
201. **"Fail honestly" has no spelling for a function that returns a VALUE, and a hash is
     the purest case.** There is no SHA-1 digest that means "not implemented", so the
     `XeCryptShaFinal` stub's silence was not an error the caller could notice — it was
     the confident answer "your file hashes to twenty zero bytes", and the caller did the
     only sensible thing with it. Gotcha 59's rule (predicates) generalises: whenever the
     return is consumed rather than tested, implementing it is the only correct option.

From phase C part 14 (a resolve has a source, and the whole frame was out of focus):

202. **A resolve has a SOURCE as well as a destination, and ignoring it is not a
     cosmetic gap.** `RB_COPY_CONTROL`'s low three bits select the buffer being copied —
     0..3 a colour target, **4 the DEPTH buffer** — and 18.4% of this title's resolves
     (10,448 of 56,925 in B1) copy depth: three shadow cascades and the scene depth its
     depth-of-field pass reads back. Serving those from the colour target made the circle
     of confusion saturate everywhere, so the ENTIRE FRAME was composited at full blur
     strength for five phases. §6d named the gap in the session it appeared and sized it
     as "four black surfaces in a table"; nobody asked what CONSUMED them. The question
     that closes it is a census of the capture by the field you are not reading.
203. **One guest address can be two different surfaces inside one frame, and the fix is
     a wider KEY, not a rebuild.** `1439B000` is a shadow cascade's depth destination
     early in a frame and the tone map's colour output late in the same one (B1's
     `1812F000`: 890 depth resolves, 852 colour). Evicting one for the other on each
     resolve is a device-wait and a fresh bindless slot twice a frame — gotcha 192's
     descriptor exhaustion with a new cause. Put the discriminator in the key and let the
     consumer choose: a fetch says which it means in its own format field (`k_24_8` is a
     depth surface).
204. **A blur is invisible to every aggregate over pixel VALUES — gotcha 135's second
     disguise.** Coverage, mean luminance, distinct colours and the histogram are all
     nearly preserved by a low-pass filter, so `frame_compare.py` scored a uniformly
     out-of-focus frame and a sharp one **0.01 pp apart**, inside its own 1.5 pp band,
     and reported "no detectable difference" about the largest visible defect in the
     port. A blur is a statement about the spatial DERIVATIVE: mean |gradient|
     (`tools/frame_sharpness.py`) reads 1.19/1.20 against 7.64/7.67, 6.47x with no
     overlap. When an operator can see something a purpose-built metric cannot, the
     metric is measuring the wrong quantity, not reporting a small effect.
205. **An address that has been "the scene" for five phases can be the scene DEPTH.**
     `CZ_VK_FRAME_STATS_SURFACE=06BE4000` is documented here as the scene surface and was
     used for every renderer A/B in this port — and `06BE4000`/`06BF8000` are the depth's
     two tiles, `0684B000`/`0685F000` the colour's. It contained colour pixels only
     BECAUSE of gotcha 202: our depth resolve copied the colour buffer, so the label was
     confirmed every time it was checked. A defect can validate the very name it
     corrupts; the thing that separated them was a field neither had ever printed.
206. **A file-open counter stops climbing when the title stops OPENING files, not when
     it stops loading.** `#154 skeleton\childfullbody.big` was handed over as "the
     frontier — stalled, or the next thing to implement?" It is neither: `NtCreateFile`
     successes stop because the title switches to reading assets out of `.big` containers
     it already has open, and `CZ_FILE_TRACE` shows the loading running on for another
     ~40 s through the cinematic props. Gotcha 109 said a capped log line is not a count;
     this is the other half — an UNCAPPED count can still not be the quantity you want.

From phase C part 15 (the prologue's black screen, and who was asking for it):

207. **"Did we compute this black, or were we TOLD to?" is a question only an arm on the
     shader can answer, and it is the first question a black frame deserves.** Three
     defects stacked under the prologue's black screen and the bottom one was the
     guest's: its tone map sets the vignette POWER to 0 and its strength to 1.0, so
     `pow(x,0) == 1` at every pixel and the compose lerps 100% to black. Every
     instrument in this project reported a healthy chain, because the chain WAS healthy.
     One line patched in the pixel shader under `CZ_SHADER_SPV` (gotcha 128) took the
     frame from 0.00% non-black to 99.99% and showed the prologue's opening highway
     underneath. The corollary is the trap: two of the three layers were ours, and the
     first one fixed — a shader the cache did not have, 28,718 draws a run — changed
     nothing at all, which reads as "wrong theory" and was really "right defect, wrong
     layer".
208. **A freshness window is a statement about ONE consumer's access pattern, and the
     era you developed it in can hide that.** "A resolve snapshot may be one frame old"
     was written for a post pass reading what an earlier pass in the same frame
     resolved. Case Zero's title screen re-renders all three colour-grading LUTs every
     frame, so the window never bound and looked correct for five phases; the prologue's
     grade is static, the LUT stops being resolved, and the fetch silently fell through
     to guest memory — which for a resolve destination is zero, because resolved pixels
     are never written back. Ask what makes the value STALE, not how old it is: here
     nothing can, so there is no window.
209. **An identity mapping can still clip.** The window-coordinate draw path scaled by
     `2/frontBufferWidth,Height` and then set a viewport of exactly those dimensions, so
     window (X,Y) landed on framebuffer (X,Y) and every arithmetic check passed. But the
     clip volume is NDC ±1, i.e. window y = 720, and the EDRAM is 1024 rows — so a clear
     rect of `(960,0)-(1024,1024)` lost its bottom 304 rows with nothing to see in any
     coordinate. A window coordinate belongs to the SURFACE the pass renders into; when
     that can be bigger than the screen, the two are not interchangeable even when they
     divide out.
211. **A capped print is not a count, and a THINNED print is not a distribution.**
     Gotcha 109's second half, and it cost two false readings in one afternoon off an
     instrument whose own comment quoted gotcha 109. Watching a RANGE of shader
     constants, the print budget's head was consumed by the lowest registers, so
     `pc(111)` printed nothing and read as "the guest never writes the vignette
     parameters in this era" — a finding, and false. Watching those four registers
     alone, the 1-in-4096 tail sampled the `.w` lane every time and invited "every
     write is zero" off four identical lines. When the question is "which values, how
     often, per register", the instrument has to be a HISTOGRAM. Asked properly the
     answer was unambiguous and confirmed the thing it was sent to doubt.
212. **Attribute a count to the BRANCH, not to the callee.** The first XMA probe hooked
     `sub_828638D0` on the strength of the call site at `82864854` and called it "the
     finished handler". It has TWO call sites in that one function — the other is on
     the still-playing path — so it is the per-update streaming refill, and the
     counter read 284,354 where the truth was 0. Read `voice+0x120` either side of the
     call instead: the guest's own cached answer names the transition.
213. **An arm's POLARITY is a design decision, not a detail.** A null decoder that
     retires a voice's input instantly makes IsPlaying read FALSE from the moment the
     voice starts — the opposite end of the same axis from the stock runtime, which
     answers TRUE forever. Both are extremes; a hypothesis about a *completion* needs
     the middle configuration, a voice that is observably playing and then observably
     done. That is why `CZ_XMA_NULL_DECODER` has a rate.
214. **An arm that manufactures progress needs a way to STOP manufacturing it.**
     `CZ_FAKE_PRESS_SEQ` holds its last button forever (gotcha 190), so every
     observation this port ever made of the prologue was taken while the title was
     being poked with A every 8 s — and "the title froze here" and "the title cannot
     leave this screen because something keeps pressing" are the same picture. `NONE`
     is now a real entry with mask 0. NB the naive control is not the control: with
     only two A presses the run never leaves the TITLE screen, so several presses are
     load-bearing and the arm has to walk the menu first and go quiet after.
216. **A shader the cache lacks is the quietest defect in this renderer, and the cache
     is only ever as deep as the deepest run that built it.** The prologue needed one
     shader neither capture contained (part 15); Still Creek needs two more. The symptom
     is a BLACK SCREEN with no error — `[vk] no translated shader for VS <hash> — draws
     skipped`, once per hash, plus a counter nobody reads. So `grep -c "no translated
     shader"` belongs in every run's post-mortem, and `CZ_SHADER_DUMP` belongs on every
     run that might reach new ground — especially an OPERATOR run, which is the only way
     this port reaches most of the game.
217. **A live process is a dumpable artefact, and a content hash makes the dump
     trustworthy.** Two missing shaders were recovered from a RUNNING game with
     `gdb -p ... dump binary memory`, using the guest address and dword count the
     renderer had already printed, and both FNV-1a'd to exactly the hash the renderer
     computed. That check is what separates this from a hopeful memory read: a freed or
     reused buffer fails the hash instead of yielding a plausible wrong shader. Reach
     for it before asking an operator to replay half an hour of game.
215. **A release build can still carry its own logging, and one hook reads all of
     it.** This image's `sub_827877C8` is a vsnprintf with **640 distinct callers**
     feeding one formatted-string sink. Every hunt in this project so far
     instrumented the RUNTIME and inferred the title's state from outside; the title
     was willing to say so all along. What stops it is a debug byte per category that
     a shipped build leaves at zero — so silence from a category is evidence about
     its FLAG, never about the category (gotcha 25 again). Look for this FIRST in any
     port of a PC-hosted engine.
210. **When a picture defect resists, count the boundary instead of looking at it.** The
     shadow cascade's "48.7% pure zero" had been a picture for a phase. Counting the
     populated region per row gave three exact numbers — 512, 720, and a 64-wide strip at
     x=960 — and 720 is the presented frame's height, which named our defect
     immediately, while 480x512 and 64x1024 turned out to be the guest's own clear rects
     read straight off `CZ_VK_DRAW_PROBE`. An axis-aligned boundary is a NUMBER; eyes are
     for transforms and blurs (gotchas 135, 204), not for edges.
140. **"Which pass consumed it" is not a question a global counter can answer.** The
    renderer counted 450,488 texture fetches served from resolve snapshots and could
    not say whether the pass that writes the front buffer was one of them — which was
    the entire question. A per-pass record turns a frame into a DEPENDENCY GRAPH, and
    that graph localised a dead post-processing chain to its first broken link in one
    run.
137. **A negative control is what stops a comparison tool inventing results.** The
    frame-signature tool judged its confidence as "the gap between the best and second
    orientation, as a FRACTION OF THE SPREAD". On two real cases it was right. On the
    negative control — the title screen against the ESRB card, two unrelated images —
    it reported "THE FRAME IS TRANSFORMED: flip-horizontal, 52% of the spread", because
    four nearly equal correlations make a tiny gap a large fraction of a tiny spread. A
    SMALL SPREAD IS NOT EVIDENCE OF A CLEAR WINNER; it is evidence that nothing
    discriminates. The fix is two gates — an absolute correlation floor and an absolute
    gap — and the general rule is that a comparison tool needs a pair of inputs that
    SHOULD NOT match, or its confident answers are unfalsifiable.
138. **Normalise by the geometry, not by the content.** Cropping to the non-black
    bounding box looked like the obvious way to compare frames of different sizes, and
    it destroyed the comparison: capture E's shots are mostly black, so the bbox finds
    the LOGO rather than the screen, and two images then get squashed to the grid by
    different amounts. Cropping both to the largest centred 16:9 rect instead — the
    game area's real geometry, checkable on E4, the one shot bright enough to reveal it
    — took the correlation from 0.067 to 0.947.
136. **A texture's component SWIZZLE is runtime data, so the runtime must apply it.**
    The Xenos fetch constant carries it in dword3; a shader compiled without the fetch
    constant cannot bake it in. Ignoring it makes a single-channel font atlas sample
    alpha as a constant 1.0, so every glyph is fully opaque and text renders as solid
    blocks of the right size in the right place — which reads as a font bug rather than
    a texture-decode one. Free to fix: a VkComponentMapping on the image view.
135. **An aggregate over pixel VALUES is blind to a transform of the picture.** The
    whole frame was rendered vertically mirrored for the entire phase — a Xenos vertex
    shader emits D3D-convention clip coordinates (+y up) and Vulkan's NDC is +y down —
    and not one instrument here could see it, because a flip preserves coverage, mean
    luminance, distinct-colour count and the full histogram EXACTLY. The purpose-built
    A/B metric scores a flipped frame as identical to a correct one. It took an
    operator looking at the Blue Castle Games logo and saying "that is upside down".
    Catching a transform needs a REFERENCE (the E screenshots, or eyes), never a
    statistic — and this is also why "the content is in the upper-left corner" was
    recorded three times without being diagnosed.
133. **One frame of an animated scene is ONE SAMPLE — and that applies to LOOKING, not
    just to measuring.** Three runs of an identical binary and an identical shader
    filter produced three completely unrelated pictures of this title screen, one
    nearly black. A per-shader bisection built on single snapshots duly localised the
    exploded geometry to a shader whose control — the same shader recompiled unmodified,
    byte-identical SPIR-V — renders just as cleanly. A picture feels like direct
    evidence in a way a percentage does not, which makes this the easier trap of the
    two, and it is the THIRD claim this phase had to retract to the same cause.
134. **Before debugging an animated scene visually, make it deterministic.** A pinned
    camera, or a guest clock advanced per frame rather than from the host TSC. Without
    it a bisection cannot converge, because its evidence is resampled every run.
117. **The picture is the one claim that needs an image, so make it self-servable.**
    Every other gate in this project is a log diff. Dumping frames AND every resolve
    snapshot from a headless run is what turns "does it look right" from an operator
    task into something checkable in the session that caused it — and the per-snapshot
    dump is the only instrument that can tell an early wrong pass from a late one,
    because the frame is the last link and a wrong frame is consistent with both.

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
- `docs/` — **`xenia-capture-analysis.md` is the numbered findings ledger and the first
  thing to read**; `big-archive-format.md` is the cracked container format;
  `xtr-decoder.md` is the GPU stream format + the determinism method;
  `bootstrap-2026-08-04.md` is the day-1 findings record,
  `xenonrecomp-upstream-bugs.md` the local recompiler patches,
  `xenia-capture-requests.md` the (unfulfilled) ground-truth requests,
  **`d3d-translation-plan.md` the renderer-architecture pivot (2026-08-06): plan,
  recon, licensing, and the per-phase build-out records — the first read before any
  renderer work**, with `d3d-kickoff.md` / `d3d-phase-c-kickoff.md` /
  `d3d-phase-c2-kickoff.md` / `d3d-phase-c3-kickoff.md` /
  `d3d-phase-c4-kickoff.md` /
  `d3d-phase-c5-kickoff.md` /
  `d3d-phase-c6-kickoff.md` /
  `d3d-phase-c7-kickoff.md` /
  `d3d-phase-c8-kickoff.md` / `d3d-phase-c9-kickoff.md` /
  `d3d-phase-c10-kickoff.md` / `d3d-phase-c11-kickoff.md` /
  `d3d-phase-c12-kickoff.md` /
  `d3d-phase-c13-kickoff.md` /
  `d3d-phase-c14-kickoff.md` /
  `d3d-phase-c15-kickoff.md` / `d3d-phase-c16-kickoff.md` /
  **`d3d-phase-c17-kickoff.md` (current)** the hand-offs,
  each superseding the last,
  `phase5-3d-plan.md` the superseded PM4-side plan for the 3D background (its Step 0
  instrument and Step 1 findings survive),
  `runtime-plan.md` the phase plan, `phase1-notes.md`, `phase3-notes.md` and
  **`phase5-notes.md`** the
  per-phase records (what the runtime work found that neither the plan nor the
  kickoff predicted), `phase1-kickoff.md` / `phase3-kickoff.md` /
  **`phase5-kickoff.md`** the per-phase hand-off prompts. **Read
  `docs/phase5-kickoff.md` before starting phase 5.** Each kickoff's most valuable
  section is its list of the parts of that phase that **already exist** and would
  otherwise be rewritten from the plan text — write that section for every future
  phase.
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
**The cache is 339 and STILL CREEK needed two of them.** A1 stops at the title screen,
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
would fail the hash rather than produce a plausible wrong shader.

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

Runtime instruments, all off by default and free when off:
```
CZ_MEM_TRACE=1     every virtual-memory call with its arguments AND its answer
CZ_FILE_TRACE=1    every open/read, including the not-founds
CZ_WAIT_TRACE=1    name any infinite wait that outlasts 5 s, with guest callers
CZ_CS_TRACE=1      name the owner of a critical section a thread cannot get, every 4 s
                   of waiting. Gated on ELAPSED TIME, not on a spin count — the count
                   version fell silent the moment contended waits started parking
CZ_CS_STATS=1      every 100,000 critical-section enters, how many were contended and
                   how many reached each backoff phase. The instrument for the only
                   risk finding 41 carries (latency on ordinary locks): 2 of 1.6 M
CZ_CS_NO_BACKOFF=1 restore the pure yield spin RtlEnterCriticalSection used to do —
                   the same-binary control arm for every claim about the backoff. With
                   it on, the two threads the title blocks forever burn a core each
CZ_STALL_TRACE=N   every N-th sleep, dump the sleeping thread's guest call sites
CZ_PEEK=addr[,n]   dump guest memory as the XEX shipped it, before any guest code runs
CZ_NULL_PAGE_READABLE=1|rw   null reads succeed (as on console) / page 0 fully mapped
CZ_RING_TRACE=1    the ring words once a second, incl. the MMIO dword we do NOT use.
                   Carries `ring: waits unmet=N held=N streak=N max=N` — the brake's
                   own health, where `max` is the longest run of CONSECUTIVE ticks
                   spent on ONE wait. That is the number that separates a title pacing
                   itself (1 on the PM4 arm, 2 on the draw arm) from a ring nothing
                   will ever release (5,491, measured with CZ_ISR_SINGLE_CPU=1). A
                   release COUNT cannot do this job: its discriminator is the stall's
                   address, and phase C re-emits its hand-off block at a FIXED scratch
                   address while the PM4 arm's rotate through the ring, so the same
                   behaviour reads 100% healthy on one arm and 4.9% on the other.
                   ALSO carries `ring: chain ...` — the GPU/CPU hand-off counted link
                   by link (arms -> ints -> isr -> kicks -> walks -> ringsub), plus the
                   number of DISTINCT token-buffer pointers the loop has iterated on and
                   the engine's spin counter printed SIGNED. Read it as a chain of
                   ratios: 0.9997 / 1.000 / 0.523 with walks==kicks==drains is the
                   healthy shape, `distinct=2` with `arms` frozen is a replay. It is
                   what retired the "~300x amplification" (gotchas 161-162)
CZ_VBLANK_MS=N     interrupt cadence (default 16); the control for timing symptoms
CZ_PM4_NO_CP_INTERRUPT=1   consume the ring but never raise source 1 (the ISR control).
                   NB it cannot be used to test "is the replay the cause": the boot
                   deadlocks at boot.bct (file #5) because the protocol needs the
                   command-processor interrupt from the first frame (measured, part 7)
CZ_PM4_RESYNC=1    scan past a parser stall instead of reporting it (off on purpose)
CZ_PM4_BIN_TRACE=N  the ME predication inputs for the first N draw packets: the draw's
                   own bin MASK, the SELECT of the tile being rendered, their overlap
                   and whether the packet ran. This title splits its scene across two
                   tiles with those two registers and a THIRD of its draw packets are
                   discarded by them, so a tile that renders almost nothing is either a
                   tile the guest had nothing for or a comparison we get wrong — and
                   only the values say which (phase5-notes §6v)
CZ_PM4_BIN_TRACE_ARM=hex / CZ_PM4_BIN_TRACE_ARMMASK=hex  hold the bin trace's budget
                   until the bin SELECT (or MASK) first takes that value. Not a
                   refinement: the first 300,000 packets of our stream and B1's are
                   packet-IDENTICAL, so a trace armed at the start shows perfect
                   agreement and proves nothing. `ARMMASK=8000000F` lands in the mature
                   tiled era, which is where the two diverge
CZ_PM4_BIN_CENSUS=1  the whole-run (bin mask, bin select) -> offered/skipped table,
                   printed by the ring trace. Deliberately the SAME table
                   tools/xtr_bin_predication.py prints for a capture, because our
                   command processor is the suspect in every question about
                   predication and cannot be its own oracle. Hardware discards 0.3% of
                   draw packets; we discard 33%, all of it one mask value at the right
                   tile (phase5-notes §6w)
CZ_BINMASK_PROBE=1 the guest side of that, and all four inputs to the mask in one
                   flag: the bin-mask setter's caller census; the rect-to-bin-mask PATCH
                   pass (sub_8284A7F8) with a histogram of what it wrote, read BACK out
                   of the records rather than recomputed so the probe cannot merely agree
                   with itself; the pass's TWO INPUTS — the tile rects (`tiles=2
                   tile0=0,0..640,720 tile1=640,0..1280,720`) and a census of the
                   per-record screen extents that produced each mask; and the bin SELECT
                   producer (sub_8284A6D0), which is what `[obj+0x164]` actually holds.
                   Every report is on a 15-SECOND CLOCK, not a call count: the previous
                   version printed at call #1 and then every 20,000, so a subsystem that
                   runs a few thousand times a boot emitted exactly one line — reading
                   "ran 1 time" — and part 10 believed it (gotcha 186)
CZ_PM4_NO_SCREEN_EXTENT=1  do NOT answer the GPU's screen-extent query
                   (EVENT_WRITE_EXT event 0x1A) — i.e. the pre-part-11 command
                   processor, in which 818,507 of those packets a boot did nothing and
                   the guest's own bin-mask fix-up pass intersected uninitialised memory
                   against its tile rects. The same-binary control arm for the right
                   tile: with it on, 76% of records come back "touches no tile" and
                   32.7% of draw packets are discarded; off, 100% come back `8000000F`
                   and 0.28% are discarded, against B1's 0.3%. Applies to gpu/pm4.cpp
                   and gpu/d3d_draw.cpp together, so the two arms stay comparable
CZ_PM4_NO_PREDICATION=1  execute predicated packets anyway. An ARM, and a destructive
                   one: running a packet hardware skipped puts one tile's geometry in
                   another tile's pass and corrupts the state stream with it (a boot
                   with it on renders nothing). It exists so that "this pass had 23
                   draws" and "this pass had 900 and 877 were predicated away" stop
                   being the same picture
CZ_PM4_DRAW_TRACE=1  the raw DRAW_INDX body for the first 24 indexed draws — the
                   instrument that settled which dword carries the index buffer's endian
                   swizzle (the TOP two bits of the SIZE dword, not the low two of the
                   ADDRESS, whose bit 1 is a real address bit)
CZ_PM4_INDEX_ADDR_SWIZZLE=1  read the index swizzle off the address dword's low bits
                   again — the pre-part-9 arm, i.e. exploded geometry radiating from the
                   exact screen centre. Applies to gpu/pm4.cpp and gpu/d3d_draw.cpp
                   together, so the two arms stay comparable
CZ_PM4_FENCE_MONOTONIC=1   refuse any GPU store that moves the engine's fence
                   COMPLETION word backwards. An EXPERIMENT arm, never a fix — hardware
                   re-executes stale EVENT_WRITEs too, and a command processor that
                   second-guesses a packet's value is not a faithful one. It engages
                   hard (5,711 refusals in 90 s on the draw arm, counted on the
                   `ring: engine` line rather than by its own capped print) and the boot
                   freezes identically, which is what RETIRED "the regressing fence word
                   is what blocks the wait" (part 7). Kept as the cheap re-ask after any
                   change to segment routing
CZ_PM4_NO_STOP_ON_WAIT=1   do NOT stall the ring at an unsatisfied WAIT_REG_MEM —
                   i.e. the pre-part-6 command processor, which evaluates each wait
                   once and carries on. **The brake is ON by default since phase C
                   part 6**, because that is what hardware does and because 40 runs
                   say so: PM4 control 2,446 frames +-1 over 10 runs with the swap
                   queue's head equal to its tail 10 of 10, against a queue that
                   OVERFLOWS 10 of 10 free-running; phase C draw 3,614-3,670 frames
                   against a BIMODAL 332..3,451,841. Zero crashes and truncated=0 in
                   all 40. This flag is the same-binary control arm for every one of
                   those claims. (CZ_PM4_STOP_ON_WAIT=1 still works, so recipes
                   written before part 6 keep meaning what they said.) Until part 4
                   the brake was gated on `depth == 0` and could not affect a single
                   one of this title's hand-off waits, all of which are inside
                   INDIRECT BUFFERs — so both of its early retirements measured a
                   no-op (gotcha 151)
CZ_NO_VBLANK_GATE=1  do NOT assert bit 0 of the display controller's gate at
                   0x7FC86544 — i.e. the pre-part-5 runtime, in which the guest's own
                   vblank ISR never runs its swap-queue walker. The same-binary control
                   arm for the gate: with it on, `dev+0x4174` stays 0 for the whole run
                   and the 16-record flip queue grows to 1,540 with nothing retired
CZ_SWAPQ_TRACE=1   the swap queue once a second: the gate, the vblank tick, records
                   retired, head/tail, the head record's own surface and due tick, the
                   GPU/CPU rendezvous word at [mirror+4] and the per-CPU acknowledge
                   bitmap at [mirror+0]. head==tail is the healthy shape; a tail
                   climbing away from a pinned head is a queue of flips nobody drains
CZ_ISR_SINGLE_CPU=1  deliver each graphics interrupt ONCE, as whatever CPU the pump
                   was constructed with (2) — the pre-part-5 behaviour. Default is one
                   delivery per bit of the arm's own six-bit CPU mask, with PCR+0x10C
                   reporting that CPU, because the ISR acknowledges by clearing
                   `1 << PCR[0x10C]` and an arm naming CPU 4 could never be
                   acknowledged otherwise (and the ISR's job ring is per-CPU too)
CZ_PM4_IB_TRACE=1  the first 64 INDIRECT_BUFFER packets with their raw address/size
CZ_PM4_DUMP_TRUNCATED=path dump the first 6 indirect buffers whose walk stopped short,
                   for offline re-walking (finding 38)
CZ_PM4_IB_VERIFY=1 snapshot every indirect buffer before walking it and compare after,
                   naming the first dword that moved. The instrument that killed the
                   "the guest is writing under us" theory: 84,808 buffers, 0 dirty.
                   Doubles the reads, so read a CLEAN result as the strong one
CZ_PM4_ZERO_IS_NOP=1       read a zero dword as a 1-dword no-op. Kept as an arm, and no
                   longer interesting: the zeros were our own unwritten VdSwap padding
                   (finding 39), and B1 turns out to contain no genuine zero header at
                   all, so the capture never had an opinion either way
CZ_NO_SWAP_PAD=1   leave VdSwap's 52-dword tail unwritten — the pre-finding-39 defect,
                   kept as the same-binary control arm for the load stall. With it on
                   the command processor walks the previous frame's packets
CZ_MULTIWAIT_APC=1 run pending APCs at the multi-object waits and report
                   STATUS_USER_APC. Correct NT semantics, off by default because
                   nothing has yet shown this title needs it
CZ_THREAD_TRACE=1  one line per guest thread with its HOST thread id, so gdb's stacks
                   can be joined to our logs (also implied by CZ_WAIT_TRACE/CZ_CS_TRACE)
CZ_ISR_TRACE=1     the scratch mirror the guest ISR reads, at each interrupt
CZ_ARG_PROBE=1     the guest-function argument probes in runtime/cpu/guest_probe.cpp
CZ_QUEUE_PROBE=1   the audio work-queue drain (sub_828576D8) and its seven call
                   sites — the instrument that showed A1's position 93 is behind a
                   failure path we never take (finding 49). Reports the first entry
                   of each, then goes quiet
CZ_JOBQ_PROBE=1    the graphics command-stream interpreter (sub_8284B568) on entry:
                   its shared object's callback/cursor state and the token buffer it
                   is about to walk. The last line before a crash IS the fatal call
CZ_FENCE_PROBE=1   the WHOLE producer side of the D3D fence/callback protocol, in
                   one flag and on BOTH arms: the fence-block emitter (828459D0),
                   the segment submit and its worker-vs-ring fork (82845AC0), the
                   close/kick (82845DE0), the callback armer (82845BA0) and the
                   graphics ISR itself (82844D38). Capped at 40,000 lines shared.
                   Run it on the PM4 arm AND on CZ_D3D_DRAW and DIFF — that
                   comparison found the missing worker kick after three sessions of
                   hypotheses about interrupt races. Session 15 added the CONSUMER
                   half, which is what the producer side alone could never show: the
                   sentinel handler / only decrementer (8284A960), the frame-end async
                   submit and only incrementer (8284B9C0), the counter spin itself
                   (82846210) and the ring submitter (828455C0) — and every cursor
                   argument is labelled SCRATCH or not, because "who reads what this
                   emits" is unanswerable from a bare address.
                   Session 16 added the three fields that retired phase C part 3's
                   ranked hypotheses: `[fence] submit` prints the fork's real inputs
                   (`incr` = r7, the counter delta, and `queue` = r8, which token
                   stream), and `[fence] kick` is a hook on sub_8284AAD0 ITSELF — the
                   ISR callback that pushes a token buffer onto the D3D worker's job
                   ring — printing the buffer pointer and flagging a kick that repeats
                   the previous one. Two arms, one diff: arms:deliveries is 768:766 on
                   the control arm and 12:856 on the draw arm
CZ_FENCE_RINGSUB=N how many sub_828455C0 calls print EVERY entry of their submission
                   list (default 4000), rather than the first two dwords of the first
                   eight. A replayed segment states its own identity nowhere else —
                   this is what showed the control arm submitting each frame's
                   arm-carrying segments about twice and the draw arm submitting one
                   of them 1,100 times
CZ_D3D_NO_RESERVE_KICK=1  suppress the guest's segment close/kick when the reserve
                   fires mid-redirect — the pre-fix arm for phase C part 2. With it
                   on the boot deadlocks at cinematics.big again (measured: file #56
                   vs #60, 5 worker kicks vs 36,747), which is what makes the fix a
                   measurement rather than an assertion
CZ_D3D_REDIRECT_PRESWAP=1  put sub_82841AD0's callback-arm block back in the
                   private scratch — the pre-fix arm for phase C part 3. That function
                   is named "PreSwapResolve" in the Phase A table and RESOLVES NOTHING;
                   with this on, all 405 of a boot's armings land in the scratch again
CZ_PM4_MEM_WATCH=hex  every write to one guest word, from BOTH streams (pm4.cpp and the
                   phase C walker each print their own line, so the log says which
                   stream wrote it). Pointed at the ISR mirror's callback slot it is
                   what proved the command processor was replaying the hand-off block
                   2.7 million times against the guest's 405 armings
CZ_FENCE_PROBE=N   as CZ_FENCE_PROBE=1, but N sets the shared line budget. Set it high:
                   a stall this probe exists to explain is at the END of a boot, and a
                   saturated budget is a floor, not a count (gotcha 109)
CZ_CRASH_TEST=nullcall  call through a zero ctr on purpose, to prove the crash
                   reporter names it. A self-test, not an arm — it announces itself
                   and the crash it causes is deliberate (finding 40)
CZ_KCALL_WHO=A,B   dump the guest call stack the first time these imports are called
CZ_AUDIO_TRACE=1   XMA context allocation + every 512th driver frame WITH its peak
                   amplitude, so "the pump runs" and "the game makes sound" stay
                   separable
CZ_AUDIO_FRAME_US=N  the driver frame period (default 5333 = 256 samples @ 48 kHz)
CZ_NO_AUDIO_PUMP=1 register the client but never invoke its callback — the control
                   arm for every claim about driving the audio callback
CZ_FAKE_START_MS=N synthetic press every N ms. A MEASUREMENT ARM, NOT A
                   FEATURE — it manufactures progress, so it announces itself on
                   every press and must NEVER be on for a gate run (gotcha 78).
                   Kept now that real input exists: it is the control for "was it
                   really my press that moved the boot"
CZ_FAKE_PRESS_SEQ=START,A,A  which buttons that arm sends, one per interval, HOLDING
                   the last rather than wrapping. Unset = START every interval, as
                   before. It exists because everything more than one menu level past
                   the title was unreachable headless and therefore unmeasurable
                   (gotcha 190): with START,A,A the boot walks title -> logo -> menu ->
                   loading screen with no operator. Names: A B X Y START BACK UP DOWN
                   LEFT RIGHT NONE. **NONE is the arm's own control** — mask 0, so a
                   sequence can walk to a screen and then go quiet. Without it the
                   title is being poked every 8 s for the whole run and "it froze
                   here" cannot be told from "something keeps pressing at it"
                   (gotcha 214). It takes about TEN A presses to reach the prologue,
                   so `START,A,A,NONE` merely parks on the title screen
CZ_XMA_PROBE=1     the guest's own audio state, on a 5 s clock: the IsPlaying
                   predicate (sub_82862A90), the per-context "has it run dry" test
                   (sub_8285EFE0, which reads the input-buffer-VALID bits at
                   dword0 bits 20/21), the per-update edge detector (sub_82864808,
                   counted off voice+0x120 either side of the call), and the raw XMA
                   context words with the hardware kick bitmap beside them. The
                   instrument that turned "there is no XMA decoder" from a statement
                   about our silence into one about what the GUEST observes: nothing
                   here ever clears an input-valid bit, so every voice the title has
                   started is still playing for the life of the process
CZ_XMA_NULL_DECODER=1  AN ARM, NOT A FEATURE: a decoder that consumes its input and
                   produces nothing, so voices can finish. Announces itself on every
                   run and must never be on for a gate run. It is what REFUTED the
                   prologue's audio hypothesis — with it on, voices demonstrably
                   start and stop (19 start / 18 stop edges) and the prologue is
                   frame-for-frame identical
CZ_XMA_NULL_DECODER_MS_PER_PKT=N  its rate, in ms of audio per 2048-byte packet
                   (default 40, derived from the contexts' own declared 48 kHz and
                   subframe_decode_count=4). 0 retires the whole buffer instantly,
                   which is a DIFFERENT arm: a voice is then dry before anything can
                   poll it, so IsPlaying reads FALSE always — the opposite extreme
                   from the stock runtime rather than the middle (gotcha 213)
CZ_GUEST_LOG=1     the ENGINE'S OWN debug printf. sub_827877C8 is a vsnprintf with
                   **640 distinct callers** handing its result to sub_828223A0, and
                   hooking that one function makes the title narrate itself. It
                   prints nothing today and that is checked, not assumed — the strong
                   PPC_FUNC is in the object file, so it is the CALL SITES that are
                   gated, each on a debug byte a shipped build leaves at zero.
                   Raising those flags is the open work (gotcha 215). `game:\cl.txt`
                   is NOT the switch: sub_82482E50 reads it as a CHANGELIST NUMBER
CZ_PM4_CONST_WATCH=<hex>[-<hex>]  a per-register value HISTOGRAM for one shader
                   constant register or a range of them, on a 15 s clock. Not a
                   sample: the sampling version read the same registers wrong twice in
                   one session (gotcha 211). It answers "which values does the guest
                   write here, how often" — and a count of ZERO over an era is the
                   finding, which no sampling of the value can produce
CZ_PM4_CONST_WATCH_FRAME=N  hold that report until frame N, because the era that
                   matters is never the boot (gotcha 139)
CZ_PM4_CONST_WATCH_ZEROS=1  restrict it to zero writes — what this instrument did
                   when it only had one job ("who zeroes this register mid-frame")
CZ_SAVE_DIR=path   where saves live (default: a SIBLING of the package directory,
                   assets/save/ — never inside assets/game/, which is extractor
                   output). An EMPTY save root is part of the A1 gate's configuration
                   (gotcha 106): A1 was captured with no save present
CZ_NO_WINDOW=1     no window, no present seam, no pad — XamInputGetState answers with
                   its documented neutral pad. The same-binary control arm for every
                   phase 3 claim. (`cmake -DCZ_WINDOW=OFF` is the build-time form,
                   for a machine with no SDL.)
CZ_VKDRAW=1        phase 5's renderer. OFF by default, so the same binary is also the
                   phase 3 binary — the control arm for every renderer claim
CZ_SHADER_DUMP=dir one file per distinct microcode blob at IM_LOAD, named by the hash
                   the renderer looks up. The input to tools/build_shader_spv.sh
CZ_SHADER_SPV=dir  override the shader cache location
CZ_VK_STATS=N      the renderer's named-counter block every N frames. Every path that
                   declines to draw something has a counter, because a renderer that
                   draws 80% of a frame looks exactly like one that draws all of it
CZ_VK_FRAME_DUMP=dir   every 64th presented frame as a PPM — the renderer checked
                   WITHOUT a window, which is what makes the E-screenshot comparison
                   self-servable instead of an operator task
CZ_VK_SNAP_DUMP=dir    EVERY resolve snapshot of one frame. The frame is the last link
                   in the chain, so a wrong frame is consistent with any pass being
                   wrong; this is the only instrument that says which
CZ_VK_SNAP_FRAME=N which frame that is (default 600). It was a hardcoded 600 for as long
                   as the instrument existed, which was fine while every question was
                   about the title screen and useless the moment one was not
CZ_VK_FRAME_DUMP_EVERY=N  the frame-dump interval (default 64). The save-slot panel the
                   synthetic-input arm walks THROUGH appears in exactly ONE frame of a
                   180 s boot at 64
CZ_VK_SKIP_TEX / CZ_VK_ONLY_TEX=<hex[,hex]>  render all but, or only, the draws whose
                   first bound texture is at that guest address. The bisection arm one
                   level below CZ_VK_ONLY_VS, because a UI compose is a hundred quads
                   sharing two shaders and the SHADER is not what distinguishes them.
                   This is how a rectangle on screen gets an identity: skip an address
                   and look at what vanished. `CZ_VK_SKIP_TEX=0364B000` deletes the
                   new-game screen's three black panels and reveals three correct
                   thumbnails underneath
CZ_VK_TEX_CENSUS=1 per texture ADDRESS: uploads, how many came out entirely black,
                   fetches served from a resolve snapshot, and fetches that fell back
                   because the snapshot was too old. Off by default because the snapshot
                   column is hit ~500,000 times a run (gotcha 7)
CZ_VK_TEX_REFRESH=<hex[,hex]>  re-read those textures' pixels on EVERY fetch, into the
                   SAME image and slot (the dimensions are part of the cache key, so
                   updating in place is exact and needs no allocation). The arm for "we
                   cached a texture the guest is still writing" — and on the garbled
                   glyph atlas it engages 2,250 times and changes nothing
CZ_VK_TEX_DUMP=dir + CZ_VK_TEX_DUMP_ADDR=<hex[,hex]>  the UNTILED bytes of a texture as
                   a greyscale PGM. It separates "our untiling scrambled this" from "the
                   texture is fine and the draw samples it wrong", which are different
                   subsystems — and a human can tell a page of glyphs from a page of
                   noise instantly, which no aggregate over it can
CZ_VK_RESOLVE_TRACE=1  each resolve's destination, SOURCE (colour or DEPTH), extent and
                   clear bits, against the front buffer VdSwap named. The trace that
                   found finding 5 below
CZ_VK_SMALL_EDRAM=1  the pre-part-14 EDRAM stand-in: 1280x720 instead of 1280x1024, and
                   snapshots clamped to it rather than sized to the destination SURFACE.
                   Both numbers used to be the presented frame's, which is right for
                   every pass that happens to be screen-sized and wrong for the one that
                   is not — this title's shadow cascade is declared 4096x1024 and fetched
                   629,023 times a boot, and it was being stored 1280x720 with its bottom
                   304 rows never rendered at all. NB the fix is committed on MECHANISM
                   (a 4096x1024 texture cannot be sampled out of a 1280x720 image), not
                   on a measured picture improvement — shadows still do not appear
CZ_VK_NO_DEPTH_FETCH=1  serve EVERY depth-format fetch the 1x1 white dummy, i.e. nothing
                   is occluded anywhere. An ARM for "is this dark mark a shadow or the
                   surface's own texture", and deliberately NOT called "no shadow": this
                   title has two depth consumers and it hits both, so it also re-blurs
                   the whole frame exactly as the pre-part-14 renderer did (which is a
                   free second confirmation of §6ae). To isolate one consumer, name its
                   address with CZ_VK_SKIP_TEX
CZ_VK_NO_DEPTH_RESOLVE=1  snapshot the COLOUR target even for a resolve whose
                   RB_COPY_CONTROL selects the DEPTH buffer — i.e. the pre-part-14
                   renderer, in which 18.4% of this title's resolves (its three shadow
                   cascades and its scene depth) delivered the wrong picture and the
                   depth-of-field pass computed a circle of confusion out of the scene's
                   own colour. With it on the WHOLE FRAME is uniformly out of focus at
                   every depth, the community-watch sign and `POP 753` are unreadable and
                   the street bunting is gone. NB no aggregate over pixel VALUES can see
                   this (coverage moves 0.01 pp): use tools/frame_sharpness.py, which
                   reads 1.19/1.20 with it on against 7.64/7.67 with it off
CZ_VK_VIEWPORT_TRACE=1 every DISTINCT viewport setup, once each
CZ_VK_FETCH_PROBE=1    which vertex fetch slots the guest has actually populated
CZ_VK_STATE_PROBE=1    the distinct values of the state registers the renderer ASSUMES
                   rather than reads: the two constant-window bases, the render-target
                   format and the cull mode. Four assumptions checked in one run, and
                   it retired "no culling is a simplification" — this title does not cull
CZ_VK_INDEX_ENDIAN=N   force one index swizzle code for every draw. The arm that
                   retired index endianness: the packet's own code beats both overrides
                   by two orders of magnitude
CZ_VK_FORCE_COLORMASK=1  treat every draw as writing all four channels — the arm that
                   retired "38.6% of draws have an empty colour mask, so the register
                   index must be wrong" (it is a real depth-only pass; frame identical)
CZ_DETERMINISTIC_CLOCK=1  the guest clock advances a fixed quantum per PRESENTED
                   FRAME instead of tracking the host TSC, covering BOTH guest time
                   sources (mftb and interrupt time). A MEASUREMENT ARM — it changes
                   what the guest observes about time, announces itself, and must
                   never be on for a gate run. PARTIAL: it halves the distinct camera
                   count and takes one pair of runs from 0.2% to 49.6% identical
                   cameras, but a third run still diverges (phase5-notes §6p)
CZ_VK_FRAME_STATS=file  one line per presented frame: draws, vertices, a draw-stream
                   fingerprint, a camera fingerprint, and the output's coverage, mean
                   luminance, distinct colours and pixel hash. The input to
                   tools/frame_compare.py
CZ_VK_FRAME_STATS_SURFACE=hex  ALSO measure that resolve surface each frame. Not a
                   refinement — the metric does not work without it, because the
                   PRESENTED frame at the title screen is mostly UI and a change
                   touching 476,858 draws moved it 0.1 pp. **The scene colour is
                   0684B000.** It was quoted as 06BE4000 from phase 5 to part 13 and
                   that address is the scene DEPTH — it held colour pixels only because
                   our resolve copied the colour buffer for depth resolves too (part
                   14). Both tiles of each: colour 0684B000/0685F000, depth
                   06BE4000/06BF8000
CZ_VK_ONLY_VS=hex[,hex] / CZ_VK_SKIP_VS=hex[,hex]  render only, or all but, those
                   vertex shaders' draws — the bisection arms. NB the picture they
                   produce is a random sample of an animated scene: judge them with
                   tools/frame_compare.py, never by eye (gotcha 133)
CZ_VK_DRAW_PROBE_MINVERTS=N  bound the draw probe to meshes of at least N indices. A
                   shader's first three draws are usually its smallest, and a defect
                   that only shows on large geometry is invisible in them
CZ_VK_SHADER_CENSUS=1  draws per (vs, ps) pair. With the capture's disassembly beside
                   every blob, this is the pair that localises a shading bug
CZ_VK_DRAW_PROBE=hash  one draw's actual matrices and the vertex data it will read
CZ_VK_STATE_PROBE=1    the state registers the renderer ASSUMES rather than reads
CZ_VK_FETCH_SLOT_INVERT=1  read vertex fetch constants at 95-slot — the arm that
                   settled the fetch-slot convention unambiguously (inverted: 0.0%)
CZ_VK_INDEX_ENDIAN=N   force one index swizzle code for every draw
CZ_VK_TEX_CACHE_FIRST=1    consult the fetch-constant texture cache BEFORE the resolve
                   snapshot — the pre-part-9 lookup order, which freezes a surface at
                   whatever guest memory held the first time it was fetched. Reproduces
                   the black scene exactly (2.31% non-black against 99.4%)
CZ_VK_SNAPSHOT_MAX_AGE=N   how many frames old a resolve snapshot may be and still be
                   served (default 0 = NO limit). **1 is the pre-part-15 renderer**, in
                   which a snapshot had to have been taken this frame or last. That is
                   right for the case the mechanism was written for — a post pass reading
                   what an earlier pass in the SAME frame resolved — and silently wrong
                   for a surface the title resolves ONCE and samples forever. The title
                   screen re-renders all three colour-grading LUTs every frame so the
                   window never bound; the prologue's grade is static, so the LUT fetch
                   fell out of the snapshot path into guest memory, which is zero, and
                   §6s already proved a black LUT is a black frame. With the guest's own
                   fade patched out to make the frame visible at all, the prologue reads
                   0.00% non-black at MAX_AGE=1 against 99.99% with no limit
CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1  map a window-coordinate draw (VTE disabled) through
                   the PRESENTED FRAME's 1280x720 rather than the EDRAM's 1280x1024 —
                   the pre-part-15 renderer. The arithmetic is an identity either way,
                   so nothing looks wrong; the CLIP is not, so every window-coordinate
                   draw taller than the screen was cut off at row 719. This title clears
                   its shadow cascade with a (960,0)-(1024,1024) rect, and that strip was
                   losing its bottom 304 rows: cascade non-black 12.82% -> 13.28%, which
                   is 64x304 pixels to the pixel
CZ_VK_RECT_HALF=1  expand a rectangle list to the SAME TRIANGLE TWICE again, i.e. the
                   pre-part-9 half-covered per-pass clear. Half of every depth clear
                   missing means the previous pass's depth rejects the scene behind it
CZ_VK_NO_MSAA_WINDOW_SCALE=1  map window coordinates one-to-one on a 4x MSAA surface —
                   the pre-part-9 behaviour, in which the scene tile's clear covers 320
                   of its 640 columns. Also disables the (never-yet-exercised) tile
                   origin correction
CZ_VK_NO_DEPTH_TEST=1  draw everything regardless of depth. An ARM, never a fix: it is
                   the only cheap way to separate "this geometry was never submitted"
                   from "this geometry was submitted and rejected by depth left over
                   from another pass", which look identical in a snapshot
CZ_VK_PASS_DRAWS=N     how many of a pass's draws the resolve trace lists (default 4),
                   each with the draw's TEXTURE address. Four says what KIND of pass it
                   is; it cannot say what a 115-draw UI compose did, which is where every
                   "why is that rectangle black" question ends. NB this knob was
                   documented here from part 9 and did not exist until part 12 — the
                   count was a hardcoded literal and the variable was read nowhere
CZ_VK_RESOLVE_TRACE_PASSES=N  the resolve trace's budget, in PASSES rather than lines
                   (default 20, about one frame). Counting lines meant the budget bought
                   a different number of passes depending on the resolve order, so the
                   frame's LAST pass fell off the end. NB this is the SECOND knob
                   documented here before it existed (gotcha 193): part 9's note says it
                   put the budget in passes and the code still counted 60 HEADER lines
                   while the two follow-up lines printed uncapped forever — so a trace
                   ran out of headers and then emitted thousands of orphan input lines.
                   Really implemented in part 14; `grep -n CZ_VK_RESOLVE_TRACE_PASSES
                   runtime/` is the check that costs nothing
CZ_VK_DRAW_PROBE_COUNT=N  how many draws the draw probe prints (default 3)
CZ_VK_DRAW_PROBE_VERTS=N  how many VERTICES it prints per attribute (default 4 = one
                   quad = one GLYPH of a text run, which cannot show whether a run's
                   cells advance across the sheet). The probe also decodes every
                   COMPONENT of a float attribute rather than its first dword — printing
                   only `u` of a texture coordinate is what let two draws sampling
                   different atlases agree on every printed value — and carries the bound
                   texture, its dimensions and VGT_INDX_OFFSET/min/max on its header line
CZ_VK_NO_INDX_OFFSET=1  do NOT apply VGT_INDX_OFFSET, i.e. the pre-part-13 renderer, in
                   which every draw reads from vertex 0 of the fetch buffer. A draw
                   packet has no base-vertex field, so this register is the only way a
                   title sub-allocates one dynamic vertex buffer between draws — which
                   is how this title's ENTIRE UI works. With it on, the save-slot panel
                   shows one overlapped garbled text run and nothing else
CZ_DIGEST_PROBE=1  the file-digest check link by link, through the alias seam: the name
                   being verified with its buffer and length, the XEX resource the
                   container asks for, the engine's own string hash RECOMPUTED IN HOST
                   CODE (which makes it an oracle rather than a description — a
                   disagreement would put the defect in the recompiled hash), and the 20
                   bytes SHA1_Final actually wrote. A hook rather than a debugger
                   because gotcha 198: ctx.rN is stale mid-function
CZ_VK_NO_TEX_SWIZZLE=1  ignore the fetch constant's component swizzle, i.e. the
                   pre-fix behaviour where a single-channel font atlas samples alpha
                   as a constant 1.0 and all text renders as SOLID BLOCKS
CZ_VK_HALF_PIXEL=1 restore the -0.5 px shift the shaders' g_HalfPixelOffset used to
                   carry — i.e. the pre-part-11 renderer, in which the scene tile's
                   clear covered columns 0..638 and column 639 of the resolved scene
                   surface was BLACK. The frame's blur turns that one column into a
                   ~19 px dark band down the middle of the picture, which no aggregate
                   in this project can see and an operator sees instantly (gotcha 188).
                   The metric that CAN see it is structural: all-black columns in the
                   resolved surface, 1 with this on and 0 without
CZ_VK_NO_FLIP_Y=1  render with a positive-height viewport, i.e. the pre-fix vertically
                   MIRRORED frame. The arm for the flip that made the title screen
                   appear; note no numeric instrument in this project can tell the two
                   arms apart (gotcha 135)
CZ_VK_NO_TEXCOORD_SWAP=1   suppress the 16-bit texcoord unswizzle mask
CZ_VK_PRIM_RESTART=1   honour 0xFFFF as a strip separator. OFF because the guest
                   declares VGT_MAX_VTX_INDX=65535, i.e. 0xFFFF is a LEGAL index
CZ_VK_RESOLVE_TRACE=N  from frame N: each resolve's destination, extent, copy window,
                   clear bits and the DRAW COUNT of the pass it closes
CZ_VK_VALIDATION=1 the Khronos validation layer. Slow at ~900 draws a frame, and it has
                   twice named an API misuse that was being investigated as a renderer bug
CZ_INPUT_TRACE=1   every pad packet published to the guest, with its button mask.
                   An instrument, not an arm: it fabricates nothing, and it is the
                   witness that a real press reached XamInputGetState. Silent on a
                   keyboard-only run until a key is actually pressed; noisy with a
                   physical stick attached, because XInput's packet number moves on
                   raw jitter too and we do not filter (gotcha 102)
```

`CZ_KCALL_WHO` is the companion to the phase gate: the gate says *that* our
first-occurrence order diverges, and the most informative divergences are imports we
call which hardware never calls at all. Only the call site explains those.

A/B the renderer. **This is the only sound way to claim a renderer change helped** —
two of this phase's three "measured improvement" claims turned out to be noise from the
title screen's ANIMATED 3D background, because a single run of an unvalidated metric is
not a measurement (gotchas 50/51/86). Aggregate over the era; never compare by frame
index (gotcha 38):
```
for a in base arm; do
  (cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_VK_FRAME_STATS_SURFACE=0684B000 \
      CZ_VK_FRAME_STATS=/tmp/$a.txt timeout 85 ./cz_runtime >/dev/null 2>&1)
done
python3 tools/frame_compare.py /tmp/base.txt /tmp/arm.txt
```
Baseline band is **1.36 pp** of median surface coverage over five runs of one binary;
the tool calls anything inside 1.5 pp "no detectable difference". It has been shown
capable of failing (gotcha 30): `CZ_VK_PRIM_RESTART=1` reads 17 pp outside the band.

Check a frame against capture E, and NAME the transform if it is one. This is the
instrument the phase-5 blind spot needed: the frame was rendered vertically mirrored for
a whole phase and no aggregate could see it (gotcha 135). Exit 1 = the frame is
transformed:
```
python3 tools/frame_signature.py \
    --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" /tmp/frames/frame_000448.ppm
```
And the pixel A/B against a noise floor measured from the same runs — imported from
Fable 2, and preferred over `frame_compare.py`'s quoted band whenever there are >= 2
runs per arm:
```
python3 tools/frame_matched_diff.py --a runA1 runA2 --b runB1 runB2
```
And how SHARP the frame is, which is the one thing no aggregate over pixel VALUES can
report. A blur preserves coverage, mean luminance, distinct colours and the whole
histogram exactly as a vertical flip does (gotcha 135), so `frame_compare.py` scored
part 14's blurred and sharp arms 0.01 pp apart — inside its own band — while the
operator could see the difference instantly. Measure the spatial DERIVATIVE instead;
it separated those arms 6.47x with no overlap:
```
python3 tools/frame_sharpness.py /tmp/dump_base1 /tmp/dump_base2 /tmp/dump_arm1 \
    /tmp/dump_arm2 --stats /tmp/base1.txt /tmp/base2.txt /tmp/arm1.txt /tmp/arm2.txt
```

Census a capture's RESOLVES by source and destination. **18.4% of this title's resolves
copy the DEPTH buffer, not a colour target** — its three shadow cascades and its scene
depth — and our command processor read that field nowhere until part 14, which is what
made the whole frame uniformly out of focus. It is also the tool that named the scene's
real colour address:
```
python3 tools/xtr_resolve_census.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
```

Disassemble the guest image. **Reach for this before reading `ppc/`** — a recompiled
function is a translation, and most questions ("what writes this field", "which branch
does this predicate take", "how many arguments does this call site really pass") are
about the original. The host toolchain cannot do it: no PowerPC target in `objdump`, no
`-b binary` in `llvm-objdump`, and `llvm-mc` silently loses instruction alignment on
the first VMX128 encoding it does not know:
```
python3 tools/gdis.py 8284B568 --count 120        # a function
python3 tools/gdis.py 8284B6C0 --to 8284B710      # a window around a faulting insn
python3 tools/gdis.py --find-uses 0x7FEA1800      # every lis/addi pair building a
                                                  # constant, with context — a 32-bit
                                                  # constant is never one instruction,
                                                  # so grepping the image misses them
```

Re-derive the save/restore helper addresses:
```
python3 tools/find_save_restore.py assets/game/default_image.bin
```

Read a GPU capture (`tools/xtr.py` is the format; the rest are thin CLIs over it —
see `docs/xtr-decoder.md`):
```
python3 tools/xtr_walk.py stats  "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/xtr_walk.py limits "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/xtr_pm4_census.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" --verify
python3 tools/xtr_determinism.py \
    "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" \
    "Xenia logs/gpu_B1b_boot_repeat/58410A8D_stream.xtr" --labels B1 B1b
```
`--verify` is the only check in the census that *can* fail — always pass it.

Replay the ME's bin-predication rule over a capture. **This is the oracle for any claim
about what our command processor discards**, and it needs no emulator: Xenia records a
`PacketStart` for every packet BEFORE evaluating predication, so the capture contains
the packets hardware skipped. Compare its pair table against `CZ_PM4_BIN_CENSUS=1`'s —
hardware discards **0.3%** of this title's draw packets and, since part 11's screen
extent, so do we (0.28%; it was 33% — gotcha 178, and gotcha 185 for the cause):
```
python3 tools/xtr_bin_predication.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" --per-select
```
`--trace-window N --trace-arm-mask 8000000F` prints the capture's own stream-order
window — the exact twin of `CZ_PM4_BIN_TRACE` + `CZ_PM4_BIN_TRACE_ARMMASK`, and the
comparison that named part 11's defect in four lines (gotcha 187). It stops at the
budget, so its census is over a PREFIX and says so.

Check the command processor against the boundaries hardware itself used. The first
covers each packet's LENGTH (24.5 M packets); the second covers a WALK — every indirect
buffer's start address and every internal boundary, chained from its first dword (28,726
buffers), which is the half a per-packet check structurally cannot see. **Exit 1 = our
parser would desync on a real stream:**
```
python3 tools/pm4_packet_lengths.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/pm4_indirect_walks.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
```
Both passing means our *arithmetic* is right. It says nothing about whether the bytes
we walk are the bytes hardware walked — that was finding 39's whole lesson (gotcha 88),
and the live counter for it is `ring: indirect buffers truncated=` in `CZ_RING_TRACE`,
which must be **0**.

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

## Current status & next steps

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

Next, in order:

0. ~~**GET A HEADLESS RECIPE THAT SKIPS A CINEMATIC.**~~ **DONE — the recipe is in
   the Commands section above and it reaches live gameplay with no operator.** START
   skips a cinematic; the Zombrex tutorial's second page needs D-pad LEFT then B. Every
   gameplay item below is now self-servable.

1. ~~**CINEMATICS NEVER END**~~ **RETRACTED THE SAME NIGHT IT WAS WRITTEN.** Later in
   the same operator session, cinematics played through and returned control cleanly —
   the Katey Zombrex grab, and the bike-frame delivery to the safehouse. The claim was
   generalised from two failures and is false as stated. What is actually true:
   **SOME cinematics fail and most do not** — and the list shrank twice more while it
   was being written. The COMBO-WEAPON one (camera parked, HUD gone, and skipping it did
   not award the weapon) **now plays properly and awards the weapon**. So the only
   confirmed remaining failure is the PROLOGUE's, which is black with the camera frozen
   at the first frame; Katey Zombrex, the bike-frame delivery and the combo weapon all
   complete.
   **What fixed the combo weapon is NOT known.** The plausible candidates are the same
   session's shader-cache completion (337 -> 353) and the bindless-heap raise, and
   nothing isolates them. Do not record either as the cause; the honest statement is
   that it was broken on the old binary and works on the new one. If it matters, the
   arm is `CZ_VK_MAX_TEXTURES=4096` — which reproduces the old heap — against a cache
   trimmed back.
   And a large share of the "cinematic is broken" evidence was never cinematics at all:
   the operator established that a black screen after a cinematic was the VIEW-DEPENDENT
   BLACK (item 1c) seen through a camera the save prompt and tutorial had locked. Two
   symptoms, one of them borrowed. Re-derive this item from scratch before working it —
   the surviving question is why those specific three fail, not why "the trigger is
   missing everywhere".
1c. **A VIEW-DEPENDENT WHOLE-FRAME BLACK, and it is now the top rendering defect.**
   Looking at the gas station (and at least one spot in the Quarantine Area) turns the
   ENTIRE frame black; turning away restores it instantly. It absorbed three separate
   "black screen" reports before the operator noticed the camera dependence. **Missing
   shaders are ruled out** — a run with `no translated shader` = 0 still does it.
   Leading hypothesis: the AUTO-EXPOSURE chain. A whole-frame, instantly reversible,
   view-dependent black is what a degenerate scene-luminance measurement produces — a
   bright emissive surface driving the 64x64 luminance reduction to an inf/NaN and
   collapsing the tone map's exposure. Supporting evidence from the other end: when the
   bindless heap was exhausted and the scene filled with WHITE dummies, the frame washed
   out, so exposure demonstrably tracks scene content. `CZ_VK_SNAP_DUMP` dumps that
   luminance chain and is the direct check.
1d. **The prologue-vs-later cinematic split may be a CLOCK problem, untested.** The two
   failure modes are opposites — frozen at the first frame, or (apparently) jumping past
   the end — which is what a timeline driven by an unclamped wall-clock delta does either
   side of a long load. `CZ_DETERMINISTIC_CLOCK=1` advances the guest clock a fixed
   quantum per presented frame and is the arm that tests it in one run. NB the "jumped
   past the end" half is itself uncertain: the operator first read auto-skips and then
   retracted them (see 1's retraction). Retired with arms and not to be re-bought: not audio, not a deadlock, not
   our synthetic input, not a missing import, not the renderer. Start from the SKIP
   path — find what it calls to end a cinematic, then ask who else should call it and
   what condition they are waiting on. `cCinematic`, `cCinematicsItem`, `cCineMovieEvent`,
   `cCineBackendMovieEvent`, `cMissionCinematic` are all named in the image with their
   source paths, and `CZ_GUEST_LOG` is already wired for the day the debug gates go up.
   **And skipping is not a workaround for PLAYING**: the operator reports that skipping
   the combo-weapon cutscene does not award the combo weapon, which is the same defect
   from the other end — the completion is what grants the reward. That puts a floor
   under how much of the game is reachable until this is fixed.

2. **THE PROLOGUE — the search space is now much smaller** (see part 16 above). It is
   not audio, not a deadlock, not our synthetic input, not a missing import and not the
   renderer, all with arms to show for it. Three lines, cheapest first: **raise the
   engine's debug-log gates** so the title says what state it is in (`CZ_GUEST_LOG` is
   already wired; the tutorial gate is the global at `0x829EC974`, the cinematic ones
   are object-relative); **instrument the cinematic system directly** (`cCinematic`,
   `cCineMovieEvent`, `cMissionCinematic` are all named in the image, with their source
   paths); or **diff what the guest does per frame either side of frame ~974**, since
   the loading screen and title screen both animate and only the world does not. The
   no-fade shader arm (`CZ_SHADER_SPV` + one line in `ps_114c4965eaabd54c`, §6af) is how
   you watch the scene while it is faded out. **Operator intel: after a new game the
   real game plays two cinematics with loadings between them and then PAUSES to show a
   tutorial** — so a frozen world is a state the game legitimately enters later, and
   there is a known-good sequence to compare against.
1b. **THE SAVE FAILS ON ONE UNHANDLED XAM MESSAGE — measured end to end.**
   `[xam] no handler for app FB message 000B0008 (8-byte buffer) — returning E_FAIL`,
   that E_FAIL completes an overlapped with `0x80004005`, and the save's poll reads it
   (`[save] XGetOverlappedResult(ovl=A3EDD414) block{result=80004005 ...} -> 2147500037`)
   at `825D6094`, where the guest accepts ONLY 0 or 996 and tears down on anything else.
   The content overlapped is innocent — it reads 0. Its sender is
   `sub_825D7CA8(dwordA, dwordB, overlapped)`, which posts the 8-byte pair as XGI
   `0x000B0008`, returns 1627 on a negative result and 997 when given an overlapped;
   callers `sub_825C4400` <- `sub_825C4190` / `sub_825C86A0`. `docs/phase1-notes.md`
   already put `000B0008` among the LOCAL XGI messages (everything past it is Live), so
   this is implementable rather than a gap — but **derive its two dwords from those
   callers before writing a handler** (gotchas 5/59/201: this is a message whose result
   the guest tests, so a wrong "success" is worse than the honest E_FAIL).
   **RETRACTED on the way**: this failure was first read off `CZ_KCALL_WHO`'s teardown
   backtrace as "the CONTENT overlapped poll returns a bad value". A live read of that
   block showed all zeros, and the probe then named a DIFFERENT overlapped. A backtrace
   names the branch, not the datum it branched on.

2. **XAM ordinal `0x271` is resolved on the save-LOAD path and we answer NOT_FOUND**
   (`docs/phase3-notes.md` finding 51). With A3's real save installed, our content layer
   enumerates it correctly and the title reaches the save-slot panel — then labels SLOT 1
   `Damaged Content` and puts up `Load failed! Please check your storage device and try
   again`, having never opened the file. `imports.cpp`'s `kResolvable` is the SEVEN
   ordinals A1 resolves, and A1 was captured with no save; A3 resolves an eighth. Do NOT
   mint a stub for it blind (gotchas 59/201) — name it from the guest's call site first,
   and note the profile-signature question is separate. This also CLOSES part 12's black
   panels: they are the save's thumbnail, and black is correct for a slot with no valid
   content.
3. **THE SHADOW CASCADE IS STILL HALF EMPTY, and part 15 halved the question.** The
   operator's top report on the running build is "no shadows anywhere". Part 15 counted
   the empty region's exact boundaries — rows 0..511 fully populated, rows 512..719 in a
   64-wide strip at x=960..1023, nothing below 720 — fixed the one boundary that was
   ours (the window-coordinate clip at 720, worth exactly that strip), and named the
   title's own clear rects with `CZ_VK_DRAW_PROBE`: **`(0,0)-(480,512)` and
   `(960,0)-(1024,1024)`**, z=1.0, compare func ALWAYS. Those do not cover a 1024x1024
   map and nothing this renderer does causes it. Three readings, all testable: 480x512
   is a PIXEL extent wanting a x2 somewhere (the pass reports `msaa=0`, so our 4x
   scaling does not apply — check what the guest thinks that surface's sample extent
   is); the "cascade" is really several smaller maps packed into one surface (four
   cascades are resolved per frame and it is 4096 wide); or the uncleared region is
   never sampled and the shadows fail elsewhere. Test the third first, because it is
   free: the consumer's fetch coordinates say which part of the map it reads
   (`CZ_VK_DRAW_PROBE` on the pass that fetches `1439B000(depth)`, 629,023 fetches a
   boot). Do NOT judge the shadow lookup until the map is right.
3b. **THE BINDLESS HEAP — MITIGATED (4096 -> 65536), NOT YET FIXED.** Confirmed
   working: a Still Creek session that reached **4,522 slots** reports `bindless heap
   full` ZERO times and the white buildings are gone. That is past the old cap, so the
   same session would have been serving dummies before. Slot recycling is still the
   real fix; a cap is only ever a bigger number. NB the operator's white BLOOD SPLATTER
   is NOT this — it keeps its splatter shape and only the colour is wrong, and the heap
   is healthy — so it is a separate texture/shading defect.
3z. **THE BINDLESS HEAP WAS EXHAUSTED IN STILL CREEK — MEASURED.** White buildings,
   white NPCs, white blood, white button glyphs, and `R->nextTextureSlot` read
   **4096** — exactly `kMaxDescriptors` — out of the operator's LIVE process with
   `gdb -p ... print`. Texture slots are handed out monotonically and NEVER RECYCLED
   (`entry.slot = R->nextTextureSlot++`); on overflow `UploadTexture` returns slot 0,
   the 1x1 white dummy, and counts `texture: bindless heap full` silently.
   **The fix is slot recycling** (an LRU over the texture cache, with deferred
   destruction so an in-flight frame cannot lose its image).
   Two things the operator's pictures add that a counter cannot. The rule is not "late
   in TIME goes white" but **"anything needing a NEW SLOT after the heap filled goes
   white"** — this title streams textures BY DISTANCE, so approaching a building
   requests a higher-resolution texture, which is a new fetch constant, a new cache
   entry and a new slot. That is why every building whitens on approach while its
   distant version was fine. And the washed-out frame and greyed HUD are a PREDICTED
   second-order effect: the dummy is white, so a scene full of dummies is a scene full
   of maximum-luminance surfaces driving auto-exposure and bloom too bright — if the
   wash survives the fix, it is a separate defect. Pictures 13-16 in
   `~/DR2CZ-troubleshooting/INDEX.md`.
   **The cheap confirming arm before the real fix**: raise `kMaxDescriptors`. If the
   buildings render, the whole causal chain is proven end to end; it is not the fix,
   because a cap is only ever a bigger number.
3e. **PP / LEVELLING AND THE CASE TIMER BAR LOOK WRONG — gameplay logic, not the
   renderer, and NOT yet separated from a HUD-drawing fault.** The operator finished
   the Fausto/Gemini escort, saw `ESCORT BONUS! 3,000 PP`, killed 41 zombies, and
   remained **LV. 1 with an empty PP bar**; the case timer bar does not appear to run.
   Money DOES update ($2,000 -> $17,000), so it is not "the HUD is frozen".
   The one piece of evidence that cuts across it: an earlier save screen read
   **`Total PP: 400`**, so PP is being tracked somewhere. That makes three live
   readings — tracked at 400, bar empty, level never rising — and they do not yet
   distinguish "PP accrues but the bar and the level-up threshold do not see it" from
   "the bar simply is not drawn and levelling is genuinely broken". Get that split
   first; it decides whether this is a renderer question or a guest-logic one, and
   they are different investigations.
   NB the guest clock IS advancing (save screens read `Day 1 - 07:08 AM` then
   `07:58 AM`), so a stopped case timer is not simply a stopped clock.
3d. **NPC PART MESHES GO MISSING, DIFFERENT PARTS ON DIFFERENT CHARACTERS.** Dick
   renders as a head and one hand; Fausto has no legs; Gemini has no hair (her dark
   arms are GLOVES and correct). These characters are assembled from separate part
   meshes — the boot loads `childface`, `childhand`, `childupperbdy`, `childfullbody`
   as distinct files — so the thing to look for is what a missing part has in common
   with the other missing parts, not what is wrong with a given character. Hair in
   particular is normally its own alpha-tested material, which is a natural candidate
   for a shader or blend-state gap.
   **Distance matters**: Dick was INVISIBLE at range and became head-and-hand on
   approach, which is the same "approaching asks for a new resource and whatever we
   lack goes missing silently" signature as the white buildings (3b) and the black
   areas (item 0's shader misses).
   **UNRESOLVED WHETHER THIS IS A CACHE MISS.** The session that found it ran 351
   shaders while 370 were on disk, and every one of the 16 shaders it reported missing
   is now translated — so some of these parts may already be fixed. Re-test on a fresh
   launch BEFORE investigating: if the parts come back, it was the cache; if they do
   not, it is a real material/geometry defect and Gemini's correctly-rendered body is
   the control sitting next to it.
3c. **The pause menu is sheared and broken in STILL CREEK and perfect in the
   SAFEHOUSE.** Same menu, same shaders, different world state — so it arrives with its
   own control, which is rare. The paper becomes a trapezoid with stray white polygons
   and thin black lines, i.e. garbage GEOMETRY rather than a texture fault. Part 13
   established that this title sub-allocates its whole UI out of ONE dynamic vertex
   buffer via `VGT_INDX_OFFSET`, so a busier scene sharing that buffer is the obvious
   place to look: an offset that drifts, or a buffer that wraps.
4. **No mipmaps have ever been uploaded** — `ci.mipLevels = 1` in `CreateImage`, every
   texture, every phase. This is the operator's "all textures seem weird grainy", and it
   is real work rather than a one-liner: the Xenos mip chain has its own address layout.
5. **The Still Creek sign's dark smear and the GAS roundel.** Neither has an identity.
   Both are `CZ_VK_SKIP_TEX` to name the address, then `CZ_VK_TEX_DUMP` to separate "our
   decode scrambled this" from "the texture is fine and the draw shades it wrong". The
   smear is NOT the untiler (0 skips in 925 textures) and NOT a shadow
   (`CZ_VK_NO_DEPTH_FETCH=1` leaves it).
6. **The last picture difference: colour is flat and green-shifted** (§6ad item 2).
   Much improved by part 14 and not closed. The tone map's LUT is what §6s proved this
   frame depends on completely.
7. **The conservative screen extent is still a placeholder** (part 11). Both tiles
   execute ~975,000 draws where hardware executes ~573,000 each. **Do not do this
   speculatively** — the cost has still not been shown to matter.
8. **A1 is exhausted as an oracle.** Its position 93 is NOT the next piece of work —
   `KeQueryBasePriorityThread` has been implemented since phase 1, and reaching it
   means reproducing an audio-subsystem FAILURE that hardware had once, late, on a
   path we do not drive (finding 49, gotcha 107). Going further needs a gameplay
   comparison built from A2 — and the run that reaches the prologue is the first this
   port has had that would exercise one.
9. **Prove the still-unexercised imports** (gotcha 67 — implemented is a prediction,
   not a result). Four of finding 34's eight have now RUN — `XamTaskSchedule`,
   `XamGetOverlappedResult`, `XMsgInProcessCall`, `XMsgCompleteIORequest`, all on the
   save-data path. Still unrun: the rest of finding 34, both of finding 36's teardown
   paths (`XAudioUnregisterRenderDriverClient`, `XMAReleaseContext` — the boot never
   shuts audio down), the save layer's own `XamContentCreateEx`/`XamContentClose`, and
   part 13's `XeCryptSha` one-shot.
10. Audio output and XMA decoding (phase 6). **DEMOTED by part 16** — it is no longer
   a candidate for the prologue blocker, so it is back to being "the game is silent".
   The kick bitmap at `0x7FEA1A80` lands in ordinary flat memory and is inert; a real
   decoder needs that aperture trapped as MMIO or the kick is written and never
   noticed. `CZ_XMA_NULL_DECODER` is the half-implementation to build on: it already
   models input consumption at a rate.
11. **A VFS gap, recorded rather than fixed** (§6ah(vi)). `VfsTranslate` returns empty
   for any guest path with no `:`, so a path with no device prefix can never resolve.
   A boot makes 29 such opens; none of those files exist under any prefix either, so
   nothing is currently lost. On console a relative path resolves against the title's
   own directory, and CLAUDE.md already warns that at least one path here is built at
   runtime (`anm_%s.big`).

## Reusability: what gets extracted, and when

Case West is the next port and this is the **second** implementation in the workspace
(Fable 2 is the first). That is what makes extraction justified now and would have made
it premature before. Two rules govern it:

> **Extract only what is proven in BOTH ports. Never extract from Case Zero alone.**

> **Extract after the second implementation forces the seam — not in anticipation of a
> third.**

**Tier 1 — hardware-defined, identical wherever you cut the layer.** Xenos microcode →
HLSL/SPIR-V (vfetch lives *in* the shader, so input layouts are reconstructed from
shader code either way); fetch-constant decode; vertex and texture format tables;
texture detiling (the Xenos address swizzle is an algorithm, not a per-title thing);
endian utilities; 7e3, D24FS8, DXN/DXT conversion; PPC/VMX helpers; the guest memory
model; XEX/STFS loading.

**Tier 2 — XDK-defined, NOT per-game.** The 360 D3D9 surface is defined by the XDK, so
build the function-signature database **keyed by XDK version** (OOVPA-style patterns)
and never hardcode per-title addresses. State vector → PSO, render and sampler state
mapping, vertex declaration handling, resolve and tiling semantics all transfer
wholesale. Case Zero (2010) and Fable 2 (2008) probably differ; **Case West almost
certainly matches Case Zero**, which is exactly what makes it the cheapest next target.
`docs/d3d-translation-plan.md`'s Phase A table is this port's contribution to it.

**Tier 3 — platform.** Kernel and XAM imports (**grown lazily — implement what a title
actually needs, never speculatively**, which is gotcha 5's rule stated as a roadmap);
XBLA entitlement handling; input abstraction; XMA/audio plumbing; save containers
mapped onto the native filesystem; achievements behind a provider interface.

**Tier 4 — host.** Graphics backend; shader hash → translation → pipeline cache.

**Never shared.** Renderer translation specifics, engine reverse engineering, hook
addresses, timing/framerate/FOV/UI patches, shader hacks.

Rules on top of the tiers:

- **Static-link shared code into each port.** No shared runtime DLL, no ABI versioning,
  no launcher dependency — each port stays independently buildable and preservable, and
  one update cannot break another.
- **Upstream universal fixes** to XenonRecomp/XenosRecomp (new instructions, jump-table
  patterns, generic shader features); title-specific corrections stay in this repo's
  config and patch tree. `docs/xenonrecomp-upstream-bugs.md` is the ledger.
- **Record the license of every borrowed component in `THIRD_PARTY.md` before the first
  line is copied** — UnleashedRecomp, ReXGlue, Xenia included. (plume is
  licence-verified MIT; only video.cpp-derived code carries GPLv3.)
- Structure the renderer so a sibling title *could* reuse it, but **do not build the
  sibling abstraction until Case West actually starts.**

**Precompile everything you can.** n = 1 per port, which a general emulator can never
assume: scan assets at install, translate shaders ahead of time, and ship a populated
pipeline database so the title starts without PSO-compilation stutter. The shader cache
(`assets/shader_spv/`, 336 blobs) already does half of this; the **125 pipelines are
still created at runtime** and are the obvious next candidate.

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
