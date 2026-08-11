# Transferable gotchas — the numbered ledger

**Split out of `CLAUDE.md` on 2026-08-08 because that file had reached 308 KB and is
loaded into every session's context whole.** Nothing here is abridged; the numbering is
unchanged and every cross-reference of the form "gotcha N" in this repo resolves here.

**Read this file whenever you are about to make a measurement claim, add an instrument,
believe a zero, or trust a number written by an earlier session.** Those are the four
situations that produced almost every entry below. `CLAUDE.md` keeps a short list of the
ones that bite most often and points here for the rest.

The list is cumulative across three ports: the ones inherited from
`~/GithubRepo/Fable2XenonRecomp` and `~/GithubRepo/Asuras_Wrath_Xenon_Recomp` are named
as such, and everything from #15 on was paid for in this one.

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
From phase C part 18 (the frame rate — and none of it was work):

218. **A CPU profiler measures the CPU, and the thread that decides the frame rate was
     ASLEEP.** `perf` over 60 s of gameplay put 38.1% of every cycle in `sub_8283C6C8`
     and 21.3% in `sub_82845160` — 73% of the process with the save/restore ladders —
     and that is the GUEST's own ring-progress busy-wait, on nobody's critical path. It
     is a true and complete answer to "where does the CPU go" and says nothing at all
     about where the FRAME goes, because a cycles profile cannot sample a thread that
     is not on a CPU. Four counters in the pump loop said what a whole session of
     profiling could not: **3.00 ticks per frame and 57% of the wall clock in one
     `sleep_for`.** When a profile's top symbol is a spin, ask what the profile CANNOT
     see before optimising what it can.
219. **An instrument that reports the HOST's own state is part of the measurement.**
     `submit` was 34% of the frame and every plan for it assumed a workload too big for
     the hardware. Splitting it showed the driver call at 0.1% and the fence wait at
     35.4%, and `nvidia-smi` showed the GPU at **P8, 210 MHz of a 2100 MHz maximum,
     15.7 W of 240 W**, its own clocks-event reason reading "Idle: Active" while the
     game rendered on it. Five sessions of profiling never asked what clock the GPU was
     at. **That part stands and is the rule.**
     **THE DIAGNOSIS DID NOT, and the correction is the more useful half.** That session
     ran overnight with the MONITOR ASLEEP; it recorded `display_active: Disabled`
     beside its own result and guessed that was the cause, but could not test it, and
     the project then adopted `sudo nvidia-smi -lgc 2100,2100` as a standing measurement
     configuration on the strength of it. Re-measured in part 20 with the display awake,
     this runtime runs at **P5, a mean of 521 MHz, 32% utilisation, 28.6 W** through
     gameplay and crowds — and `vkcube`, an ordinary presenting Vulkan application,
     settles in the same place on the same machine (P5, 510-600 MHz, 33-39%, 29.5 W).
     **The governor was never mistreating us; a blanked monitor was.**
     So the two cross-checks quoted above were both real and both consistent with the
     WRONG conclusion: utilisation matched the duty cycle and power matched the clock,
     because the clock genuinely was 210 MHz — for a reason outside the program. A
     cross-check confirms a reading, not an explanation of it, and this one needed a
     CONTROL: another application on the same machine at the same moment. That control
     costs 20 seconds (`vkcube`) and was never run.
     What survives as a rule: sample the clock and QUOTE it (`tools/gpu_clock_sample.py`)
     rather than pinning it. Pinning trades representativeness for comparability, and it
     is only legitimate when the governor is demonstrably wrong — a low clock at HIGH
     utilisation. A low clock at LOW utilisation is the governor being right, and the
     thing to fix is then the idleness, not the clock. See gotcha 231.
220. **A period built by counting ITERATIONS of a loop that also does work is not a
     period.** The vblank was delivered every N iterations of a loop whose body is a
     sleep PLUS a ring walk, so every millisecond the walk spent waiting for the GPU
     pushed the guest's next vblank a millisecond out: **40.2 vblanks/s, never the 60
     the console has, for the whole life of this runtime.** And the sharp edge — making
     the ring tick 16x faster made the vblank SLOWER (31.2/s), because more iterations
     per frame charged more of the walk against its budget. A timer must be scheduled on
     a clock. Fixing it was 2.0x, because the CP's per-frame waits are released by the
     swap-queue walker inside that very ISR, so the runtime was pacing the title off its
     own slowness.
222. **A workload profiled at one scale is not the workload.** Every performance
     conclusion this port ever reached was measured at the title screen (~2,500 draws) or
     the headless gameplay recipe (~1,930). A Still Creek zombie crowd is **4,800-6,800**,
     and at that scale the frame budget REORDERS: the GPU goes from the largest term to
     the smallest, `other` goes from 0.0% to 4.2%, `textures` from 2.5% to 10.9%, and the
     item the overnight plan ranked LAST becomes worth doing. Worse, ordinary gameplay
     sits against the title's own two-vblank cap, so a CPU saving there measures as
     exactly zero — a change can be genuinely worth 3% of the workload that matters and
     score a dead heat in the one you can reach headlessly. Before optimising, ask what
     the WORST case renders and whether your harness can reach it (gotcha 190's rule,
     pointed at performance rather than at defects).
223. **Never put a `Count()`, a `getenv` or a `std::string` on a path you are timing.**
     `Count()` here is a `std::map<std::string>` lookup. Five of them added to the SKIP
     paths of a per-draw state cache made the cached arm pay five map lookups to save
     five `vkCmd` calls, and the A/B came out a dead heat BY CONSTRUCTION — the arm was
     real, the instrument cancelled it exactly. `perf` had already shown
     `std::map::operator[]` at 0.44% and `getenv` at 0.42% of a 1,930-draw frame, which
     is 3x that in a crowd. Gotcha 7 says a probe expensive enough to stall the game
     manufactures the stability it reports; this is the same rule at the scale of a
     single branch, and it cost two false results in one day.
224. **A FIXED-SIZE per-frame allocator turns "too much geometry" into "a black
     screen", and nothing in the picture says so.** This renderer's per-frame bump arena
     was 128 MB against a true peak of 161; `ArenaAlloc` skips every draw it cannot
     satisfy, and this title's post-process chain is at the END of the frame — so an
     overrun lost the downsamples, the luminance ladder, the colour LUTs and the tone
     map, and presented a completely black frame with a correctly rendered scene sitting
     in EDRAM behind it. It was reported as a **view-dependent whole-frame black** and
     spent six parts as the port's top rendering defect, attracting an auto-exposure
     hypothesis, a shader-cache hypothesis and a bindless-heap hypothesis. "View
     dependent" was literal and benign: which way the camera points decides how much
     geometry is in the frame.
     Three transferable pieces. **Exhaustion is a property of ONE FRAME**, so a running
     total is the wrong shape — name the frame, once per frame, or the counter can never
     be joined to the frame that presented wrong. **Grow, do not raise**: a bigger fixed
     number is the same defect further away (open-items 3b says the same about a bindless
     heap), and the safe place to reallocate is the frame boundary where the command
     buffer has just been reset. And **a resource limit is a rendering defect wearing a
     disguise** — when a picture fails in a way that scales with scene complexity, audit
     every fixed-size pool before theorising about shading.
225. **A sampler normalises over the image you hand it, not over the surface the guest
     declared.** A resolve destination's PITCH and its WIDTH are different numbers —
     `RB_COPY_DEST_PITCH`'s low field is the pitch — and a snapshot built at the pitch,
     sampled by a fetch that declares the width, scales every texture coordinate by
     width/pitch. It is INVISIBLE whenever both are multiples of the tile alignment,
     which is every full-screen surface (1280, 640, 320, 160), so a renderer can be
     broadly right for phases while quietly destroying anything that is not: here it ate
     the tail of a luminance reduction ladder (80-of-96, then 40-of-64, 20-of-32,
     10-of-32, 5-of-32, compounding) and delivered the tone map a scene-average
     luminance of **exactly zero** in every frame of every era. The confirmation is worth
     copying: predict the lit-column count of each link from that one ratio and check
     five links in a row. The height needed no fix, because pitch is a width-only
     concept.
226. **A trigger fires on the metric you gave it, not on the defect you meant.** A
     "dump the frame the picture died on" instrument keyed to COVERAGE caught only
     loading screens — legitimately black, and the only thing in a gameplay run that
     trips a 0.5% coverage floor. The defect it was built for moves MEAN LUMINANCE.
     Both thresholds are now kept rather than one being redefined, because silently
     changing what an instrument means invalidates every run already taken with it.
     And the addition that made it useful: a dark episode dumps a **BRIGHT REFERENCE**
     chain from the same location seconds later, because one dark chain is equally
     consistent with "this pass is broken" and "the scene really is dark here", and only
     the pair separates them (gotcha 133 turned into a feature).
227. **A title's error message names the subsystem it BLAMES, not the one that failed.**
     Case Zero says "Load failed. File appears to be corrupt. Please check your storage
     device" when an xam ordinal it needs is not resolvable — with the save file
     untouched on disk and **never opened**. Three sessions read that text, and the
     `Damaged Content` label beside it, as evidence about the save's CONTENTS, and one
     of them built a whole theory on it (a 360 save is signed per profile, so a save
     made under another profile GUID would legitimately be rejected — plausible,
     testable only in principle, and wrong). The log said what actually happened in one
     line, and the giveaway was that no `NtCreateFile` on the save device appeared
     anywhere near the failure.
     The rule: when a guest reports a failure, find the LAST thing it successfully did
     and the FIRST thing it did not, and believe those. A title's diagnostics were
     written for a console where the runtime beneath them was correct; on a
     recompilation they describe a world that does not exist. Corollary for the fix
     side: this is why an unimplemented export must fail LOUDLY with its identifier
     (gotcha 5) — `ord=0x271 -> NOT_FOUND` is what turned a three-session-old mystery
     into an afternoon.
228. **A NESTED scope in a profiler counts its time twice, and every column still adds
     up.** `ProfScope` here accumulated inclusive time; `record` opens partway down
     `DoDraw` and lives to the end of it, so the `UploadStream` calls below it ran
     inside it and their cost landed in `streams` AND in `record`. The frame print then
     derived `DoDraw`'s residual by subtracting the named phases from the whole, which
     removed `streams` twice. Result: `record` read 11.07 ms of a 21.40 ms draw path
     and the residual read 0.91 — where the true split is 6.30 and 5.68. **An entire
     performance plan was written on that ranking**, filing its second-biggest item as
     "the cheapest item in this document".
     What makes this class hard is that nothing looks wrong. The sum of the columns
     still equals the total, because the error MOVES time from the outer scope's
     residual into an inner scope's name rather than creating or destroying any. There
     is no shortfall to notice and no counter that disagrees.
     The fix is structural, not arithmetic: every scope subtracts what its children
     consumed, so a column means "time in THIS phase and no other". Then compute the
     total as a SUM of the columns instead of measuring it separately — two independent
     statements that CAN disagree are worth more than one that cannot, and the residual
     stops being a place for errors to hide. The general rule: **a profiler is
     instrumentation, so gotcha 30 applies to it — break it on purpose and check that
     the columns move the way you predicted.** Neither this project nor the previous two
     had ever done that to their own `ProfScope`.
229. **The noise floor of a run-based A/B is measured with a NULL ARM, and binning is
     not enough on its own.** Case Zero's headless crowd recipe is 57 fixed 8-second
     steps against a boot whose depth in wall time is a distribution, so two runs of one
     binary visit different places for different durations. `tools/frame_perf_bins.py`
     handles the obvious half by comparing frames BINNED BY DRAW COUNT rather than
     averaging a run — a 6,000-draw frame is the same workload whenever it arrived.
     That is necessary and it is not sufficient. Run against a genuine null arm (a
     commit that changed only the profiler's arithmetic, so nothing that executes
     differs), individual bins still moved **−5.9% to +5.0%**, with the tool's own
     standard-error column reading as high as 8.7 sigma. The frames inside a bin are not
     independent samples: consecutive frames share a camera, a location and a thermal
     state, so the effective N is a small fraction of the frame count and any
     significance computed from the raw count is confidently wrong.
     The practical rule: **before believing an A/B built on a long scripted run, run the
     same comparison with nothing changed** and treat whatever it prints as the floor.
     It costs one extra run and it is the only thing that tells you which of your
     columns can carry a claim. This is gotcha 50's "a rate measured once is a fact
     about that afternoon" arriving in a project that had already done the binning and
     thought that was the answer.
230. **An instrument that is "off" must be free in its WORK, not just in its OUTPUT.**
     The `[psbind]` probe is gated on `CZ_VK_PSBIND`, and the gate was around the
     `fprintf`. The `snprintf` that formatted the shader hash it compares against sat
     ABOVE the gate and ran on every draw — ~6,600 string formats a frame for a
     diagnostic nobody had enabled. Same shape as the four `getenv` calls per draw and
     the `std::map<std::string>` counters beside them (gotcha 223): each is a line
     written to make an instrument available, and each pays whether or not anyone
     wanted it.
     `docs/instruments.md` opens by promising every arm here is "off by default and free
     when off". That promise is a claim about code, so it needs checking like any other:
     `grep -n 'Env(\|getenv\|snprintf' ` over the per-draw path, and confirm each is
     behind a function-local `static` or inside the gate rather than beside it.
231. **A low clock at LOW utilisation is the power governor being CORRECT — the defect
     is the idleness.** Case Zero's crowd frame leaves the GPU idle 68% of the time
     (32% utilisation), because the renderer submits its command buffer and immediately
     blocks on the fence, so our CPU and our GPU never run at the same moment. The
     driver responds by picking a mid clock, which is exactly what it should do: there
     is no deadline the GPU is missing. Forcing 2100 MHz makes that work finish in 5 ms
     instead of 18 and costs 52.8 W against 28.6 — buying back frame time that a
     PIPELINED renderer would have got for nothing, at nearly double the power.
     The diagnostic pair is the point. Clock alone says nothing; utilisation alone says
     nothing; together they separate "the hardware is being held back" from "we are not
     asking the hardware for anything". Only the first justifies touching the governor.
     Corollary, and it is how this was found: **a retirement is only as good as the
     oracle it was measured on** (gotcha 172). The overnight plan's §2a — overlap the
     GPU with the CPU, ceiling ~1.5x — was dismissed on the grounds that the GPU term
     was "mostly an artifact of the machine's power state". The artifact was the
     MEASUREMENT's, not the frame's, and retiring it revives the item at its full size:
     27.7 ms of CPU and 16.5 ms of GPU that currently run one after the other.
232. **Model knowledge about how expensive something "usually is" is not a measurement
     of YOUR code, and it can be 20-40x wrong.** A first-visit cost of 16.7 ms a frame
     was attributed to Vulkan pipeline compilation, on arithmetic that seemed to close:
     ~5 new pipelines a frame at ~3 ms each is ~15 ms. The 3 ms came from general
     knowledge of what pipeline compilation costs, and it had never been measured in
     this renderer. Timed: **0.08-0.15 ms**. The busiest window observed created 89
     pipelines and spent 11.0 ms TOTAL — 0.2% of frame time, where the hypothesis needed
     ~139 creations in a single frame.
     The tell was available before the counter and was ignored three times: the
     hypothesis was restated in three consecutive messages, each time as "still needs a
     counter", and each time the argument was made instead. It then failed a
     pre-registered prediction, and a rescue was available ("no new shaders loaded
     there") which had to be refused because it was unfalsifiable — new pipelines come
     from new STATE combinations too. **A hypothesis that has survived only inference is
     not evidence, and the third time you write "this needs a counter" is the moment to
     write the counter.** It cost twenty lines and refuted the idea on its first run.
     The general form: every "X is expensive" and "Y is cheap" carried in from outside
     the project is a prior, and priors about performance are exactly what profiling
     exists to overturn. Put a number on it before it becomes a plan.
233. **A cache's HIT RATE and its COST are different questions, and a high hit rate can
     hide an enormous one.** This renderer's per-frame vertex/index stream cache runs at
     **94% hits** in a crowd — the number anyone would quote to say it is working — and
     the remaining 6% still copies **74-77 MB every frame**, 5.6-5.9 ms, the largest term
     in the draw path. The plan had written the ambiguity down correctly ("a high miss
     rate and a high hit rate need opposite fixes") and then guessed the wrong branch
     from the hit rate alone, because 94% *sounds* like the copying is gone.
     **Count BYTES, not just hits.** A hit rate is a property of the lookups, and the cost
     lives in the misses' sizes, which the ratio cannot see. Two caches with the same
     94% can differ by two orders of magnitude in bytes moved.
     Second half of the same lesson: the profiler scope wrapped only the copy, so a hit
     never touched the column being argued about at all — nine lines of code answered
     half of what was called unanswerable-without-measurement. **Read where the timer
     starts before theorising about what the number contains.**
234. **A comparison that only ever reports 100% has not been shown capable of reporting
     anything else — SALT IT AND CHECK IT READS 0%.** A content check comparing each
     cached buffer's hash against last frame's read exactly 100.0% in every window of
     every run, which is either a real and very useful fact about the guest's geometry or
     a comparison whose two sides are the same value by construction. From the output
     those are identical. The control is one environment variable and four lines: salt
     the hash with the frame number so identical bytes MUST hash differently, and require
     the line to read 0.0%. It read 0 of 96,048.
     **The control paid for itself immediately**, which is the part worth carrying: with
     the poison arm in place the honest reading of the unpoisoned arm changed too, because
     the rounding had been hiding real mismatches — 164 of 10,154,820 repeated keys DID
     change content. Without the control the conclusion would have been "100%, safe to
     cache blindly"; with it, the conclusion is "must invalidate", which is a different
     design. This is gotcha 30 in the specific shape it takes for *equality* checks, and
     equality checks are where it hides best, because the passing state is silent.
235. **A census that only looks ONE STEP BACK measures a smaller question than the one a
     cache asks — and will understate its risk by orders of magnitude.** Part 21 measured
     how often a guest stream's bytes changed *since last frame* and got **164 of
     10,154,820, 0.0016%**, a number that reads as "essentially never". The cross-frame
     store built on it compares against the **last COPY**, which may be dozens of frames
     old, and its counter immediately reported **~20 stale streams a frame** — two orders
     of magnitude more. Neither number is wrong. An address the guest recycles for a
     different mesh after a gap is invisible to a frame-to-frame comparison **by
     construction**, and it is precisely the case a persistent cache is exposed to.
     **Match the lookback of the measurement to the lifetime of the thing being
     designed.** If a cache will hold data for N frames, a one-frame staleness census is
     not evidence about it; it is evidence about a one-frame cache. The tell is that the
     instrument's window is a parameter nobody chose deliberately — here it was
     `g_prevStreamKeys`, cleared every `BeginFrame` because that was convenient.
     The saving grace was building the guard anyway on the strength of a 0.0016% that
     "could have been zero but was not". Had the census read a true zero, the temptation
     to skip invalidation would have been much stronger, and the defect would have been
     an intermittent wrong mesh — see 233's family of caches that look fine.
237. **When frame time is clamped by a PACING FLOOR, a mean over frames measures the
     floor, not your change — read the MEDIAN and the share of frames sitting ON the
     floor.** The cross-frame store removes 5.5 ms of copying from a crowd frame.
     `tools/frame_perf_bins.py`, which reports means, scored it **+1.7% against a +1.3%
     null** and would have been read as "below the noise floor, not worth the session".
     Binned finer and read as medians it is **44 ms -> 32 ms, 27%, at ~3,700 draws** —
     and the column that proves it is neither: the share of frames within 1 ms of a 16 ms
     multiple goes from **10% to 97%**. That is the whole finding in one number. Arm B is
     free-running and CPU-limited; arm A has been pushed onto the title's own vblank floor
     and is no longer our problem.
     The general rule: a saving converts to frame rate only where the frame is **above one
     floor and within reach of the next**. At ~6,500 draws both arms were already parked
     on the 48 ms three-vblank floor, 5 ms could not reach 32, and the same change measured
     as nothing. `perf-cpu-plan.md` item 0 already said this for the TWO-vblank cap at
     ~1,930 draws; it was not generalised, and a real win nearly got filed as noise.
     **The pinned-share is also the more sensitive instrument** — it moved 10% -> 97%
     where the mean moved 1.7%.
238. **When you remove work from a timed scope, find out where the REPLACEMENT work is
     charged.** With the store on, `streams` reads **0.0%** — the copying is genuinely
     gone — and reading that alone would have claimed the full 5.5 ms. The guard hash that
     makes the store safe runs inside `UploadStream` but outside `ProfScope(streams)`, and
     `record`'s scope encloses it, so the guard's ~1.9 ms is charged to `record`, which
     duly nearly doubled. The true net was 3.3 ms, not 5.5 — a 40% error in the direction
     that flatters the change. This is the same lesson as 233's second half (**read where
     the timer starts**) arriving from the other end: there, an unread scope made a cost
     look ambiguous; here, an unread scope made a saving look bigger. A zeroed column is
     not a saving until the residual is checked.
236. **An instrument that writes FILES must complain when it cannot.** `CZ_VK_FRAME_DUMP`
     was a bare `fopen` whose failure was silent, so pointing it at a directory that did
     not exist produced an empty directory — which is indistinguishable from a renderer
     that drew nothing, and the picture check is the one gate in this project with no
     log-diff substitute (117). The same shape as 25 and 151: a path that can produce
     nothing without saying so will eventually be read as a result.
221. **A measured win can cost a gate, and the honest move is to price both.** The two
     changes above take A1's position-71 window from 1-in-10 to every run. It is a
     two-thread interleave with an identified mechanism, the stronger set-based A5 gate
     stays exit-0 with zero real windows, and `CZ_PM4_TICK_MS=16` / `CZ_VBLANK_TICKCOUNT=1`
     reproduce the strict prefix — but `kernel_call_diff.py` refuses to relax the masked
     gate for permutations ON PURPOSE, so this is a real loss and not a technicality.
     **Refined in part 19 by running it twice: those two flags REDUCE the interleave, they
     do not remove it.** Two runs of that exact configuration gave an exact prefix once
     and the position-71 three-name rotation the other time, so "restores the prefix in
     one command" was itself a single-run claim about a bimodal thing (gotcha 159). Quote
     the set-based A5 gate, which holds.
     What decided it is that both changes move TOWARD the hardware: a command processor
     that runs continuously and a 60 Hz display timer are what the console has. Record
     the unfavourable number, not just the favourable one (gotcha 160).

215. **A release build can still carry its own logging, and one hook reads all of
     it.** This image's `sub_827877C8` is a vsnprintf with **640 distinct callers**
     feeding one formatted-string sink. Every hunt in this project so far
     instrumented the RUNTIME and inferred the title's state from outside; the title
     was willing to say so all along. What stops it is a debug byte per category that
     a shipped build leaves at zero — so silence from a category is evidence about
     its FLAG, never about the category (gotcha 25 again). Look for this FIRST in any
     port of a PC-hosted engine.
     **CORRECTED IN PART 28 — "a debug byte per category" was wrong, and it was wrong
     in the direction that made this look expensive.** See gotcha 266: it is ONE byte
     for the whole engine, and it is a kill switch rather than an unset flag.
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
239. **A shipped retail executable may still contain the studio's entire debug build,
    switched off rather than compiled out — look before assuming it was stripped.**
    Case Zero's image carries `common\debugmenu\debugmenu.cpp` in its source-path
    strings, the `cDebugMenu` class, the whole menu item tree (System Menu, Chartz
    Menu, Thread Edit Menu, Performance Chartz, GPU Timing Queries, NPC To Spawn), a
    `God Mode:ON` overlay referenced by live code, and a `DebugJump` frontend screen
    **whose layout ships**: `debugjump.txt` is a real entry in `data/frontend/
    mainmenu.big` and at 4,144 bytes it is the LARGEST entry there, bigger than
    `title.txt`. 393 booleans gate all of it. One loader (`sub_824A2470`) resolves
    each BY NAME through a lookup that finds nothing in a retail build, and stores
    the answer as a byte in one contiguous struct; every consumer gates on a plain
    `lbz`/`cmplwi`/`beq`. **In a static recomp this is a one-line poke**, because the
    loader has a single caller three hops off the XEX entry point and nothing rewrites
    the bytes afterwards — so the hook has no per-frame component at all and cannot
    perturb what it reports (gotcha 7). `runtime/cpu/debug_tunables.cpp` is the worked
    example and `CZ_DEBUG_MENU=1` is the switch. **The generalisable part is the search
    order**: grep the image for source paths and menu-item strings first, then find the
    gate byte by walking `.text` for the loader's (`addi` name / `stb` offset) pairs,
    then CONFIRM each byte independently by scanning for the `lbz` consumers that read
    it back. **That confirm step is necessary but NOT sufficient, and gotcha 241 is
    the correction**: every byte in such a struct is a real tunable that something
    reads, so a reader scan cannot tell a correct address from its neighbour. Bind
    names to bytes with a dataflow simulation over the loader, not by pairing each
    name with the nearest store.
240. **Flipping a feature's gate proves the gate flipped, not that the feature appeared.**
    The three debug-menu bytes above go 0 -> 1 exactly as predicted, on a headless boot,
    with a same-binary control — and the main menu still shows only START GAME /
    LEADERBOARDS / ACHIEVEMENTS / HELP & OPTIONS / EXIT GAME. `enable_debug_jump_menu`'s
    single reader turned out to gate a text-formatting path, not the menu list. The
    `0 -> 1` transition is real evidence of exactly one thing (that the retail config
    genuinely resolves these to false, which was the load-bearing assumption) and no
    evidence at all of the thing that was actually wanted. **A flag is a cause; the
    picture is the claim** — and this is gotcha 117 arriving from a new direction, since
    the only reason the refutation was available in-session was that frames are
    self-servable.
241. **When a loader resolves a list of names, the store next to a name is usually the
    PREVIOUS name's result — bind them by dataflow, never by adjacency.** Case Zero's
    tunable loader holds each lookup's result in a register, loads the NEXT name into
    the argument register, and only then stores:

        bl   lookup          ; nameA -> r3
        mr   r11, r3
        addi r4, <nameB>     ; nameB is now pending
        stb  r11, <offA>     ; ...and this stores nameA
        bl   lookup          ; nameB

    Pairing each `addi` with the `stb` that follows it named all 387 flags after
    their neighbour — a uniform off-by-one that produced a table which looked
    perfectly regular. **Two independent-seeming checks failed to catch it.** The
    `lbz` consumer scan finds readers at both candidate addresses, because in a dense
    flag struct every byte is a real tunable something reads — so "it has readers" is
    not evidence the name is right. And reading the bytes back out of the live process
    only confirmed we had written the bytes we chose, which is a tautology, not a
    check (gotcha 240 is the same shape). The cost was an entire session: the preset
    that was supposed to open the debug menu set `enable_dev_only_debug_tiwwchnt`,
    `debug_on_controller_2_only` and `debug_show_loading_time` instead, and the
    plausible-looking "quickie routes debug input to controller 2" reading was just
    the neighbouring flag. **A check that cannot distinguish the right answer from the
    adjacent wrong one is not a check** — ask what result would refute the binding
    before trusting a scan that confirms it.
242. **A threshold fitted to a census is fitted to the population your instrument could
    REACH, not to the population that exists.** The cross-frame store's guard was exact
    to 512 bytes because a census reported every rewritten stream was exactly 80. That
    census could only observe streams rewritten between two CONSECUTIVE frames, in a
    headless recipe that walks and looks and never fires a weapon or changes a HUD
    number. The streams it could not reach were the ones that broke — a HUD batched into
    one multi-KB buffer where only the digit quads change, which the 8x64 sampling missed
    entirely, so the store served last frame's numbers (open item 00c, three sessions).
    The bound was not wrong when it was chosen; it was fitted to a keyhole. **Two habits
    fall out of this.** Before setting a threshold from measured data, ask what the
    measurement CANNOT see and whether the threshold is being asked to cover it. And ship
    a COUNTER for whatever falls outside the threshold — the raised 16 KB bound reports
    "604 streams/frame exceeded it and were SAMPLED" on every profile window, so the
    residual exposure is a number somebody can read instead of an assumption nobody
    revisits. This is gotcha 235's second instance and the first one where the fix was a
    counter rather than a better census.
243. **When the platform pins your frame time, a CPU COST is as invisible as a CPU saving
    — and that cuts both ways.** Raising the guard's exact bound took `record` from 4.8%
    to 19.3% of a crowd frame, hashing 14 MB where it used to hash 0.4, and the frame
    stayed at 32.2 ms and 31.0 fps at 6,778 draws because this title paces itself to two
    vblanks. That is the same mechanism as gotcha 237, which was learned as a reason a
    saving did not show up. Read it in both directions: it makes a correctness fix
    affordable that a spreadsheet would have rejected, and it means a real regression can
    hide until some other change lifts the frame off the floor. Quote the headroom
    (`outside`) alongside the phase percentages, because that is what says how much cost
    the floor can still absorb.
244. **A field whose value another oracle can PREDICT should be located by census, not by
    recollection.** The renderer needed the texture fetch constant's `dimension` field.
    From memory it was "dword5, bits 7..8"; it is dword5 bits **9..10**, and nothing in a
    run would have said so, because a wrong dimension does not fail — it produces a
    plausible wrong image. What found it in one run: the SHADER independently declares
    each fetch's dimension, so it partitions every fetch into classes that must differ in
    exactly the bits of that field. Accumulate the AND and the OR of every dword per
    class; a bit set in all of one class and clear in all of another is a candidate, and
    everything else varies within a class and cannot be a constant per-dimension field.
    Over 842,556 2D and 47,574 cube fetches exactly two dwords separated the classes, and
    one of the two (dword2's top six bits reading 5 = six faces) had been PREDICTED from
    published layout before the run, so the run could have refuted the whole reading.
    **The general shape: whenever two independent sources describe the same fact, one of
    them is a free oracle for decoding the other, and the decode becomes a measurement
    with a stated refutation instead of a remembered constant.** Cheap enough to be the
    default — it is one counter and one report — and it generalises to any bitfield in any
    guest structure where a second description exists.
245. **A structure field that is constant everywhere is a defect waiting for the first
    exception, and the exception arrives as undefined behaviour rather than as a wrong
    picture.** `Barrier` had `layerCount = 1` hardcoded in its subresource range, which
    was correct for every image this renderer had created for the whole of phase 5 and
    became silently wrong the instant one had six layers: five of a cube map's faces would
    never have left `TRANSFER_DST`. The presentation would have been one correct face and
    five wrong ones, which reads as a texture-decode bug and would have been investigated
    as one. **When adding the first instance of a shape a helper has never seen — the
    first multi-layer image, the first multi-mip one, the first 3D one — read that helper
    for the dimensions it assumed rather than waiting to see whether the picture is wrong.**
    The companion half is gotcha 25 in a new place: this project's logs all say
    `VK_LAYER_KHRONOS_validation is NOT INSTALLED`, so grepping any of them for `VUID`
    returns zero for the reason that a grep which cannot match is not a clean result.
    A missing validation layer is a silent removal of every check you think you have.
246. **Ship the DENOMINATOR with the counter, or the first number you publish will be a
    fact about one recipe.** Part 25 added a counter for fetches where the shader and the
    guest disagreed about a texture's dimension, read 114 against 337,602 agreements on
    the boot-to-gameplay recipe, and wrote "0.03%" into three documents and a commit
    message. The SAME BINARY on the deeper outdoor recipe declined 90,984 — and there was
    no total to divide by, because nothing counted the population at all. The 0.03% was
    not wrong; it was a fact about the safehouse, published as a fact about the game.
    This is gotcha 242's shape (a statistic fitted to what the instrument could reach)
    arriving through a different door, and the fix is mechanical: **whenever you add
    `Count("X happened")` for a subset, add `Count("X was possible")` in the same commit.**
    One extra increment on a path that already has one, and it converts every future
    quotation from an absolute count — which means nothing across recipes — into a share,
    which means the same thing everywhere.
247. **A cross-arm picture comparison needs an ADMISSIBILITY TEST it can actually run, and
    a per-frame fingerprint is what makes one enforceable.** Part 25's cube-map A/B looked
    decisive at first: matched present indices showed a mean |RGB| difference of 13-34 in
    the gameplay era. All of it was DRIFT — the two runs are at different places at the
    same index, because a fixed synthetic-input recipe reaches different depths on
    different afternoons (gotcha 75). This repo's frame-stats file already carried a
    `drawFingerprint` and a `cameraFingerprint`, so the A/B rule this project wrote down
    sessions ago — two arms are comparable only if they are two states of ONE renderer
    producing the SAME draw set — stops being a slogan and becomes a filter: of 301
    matched dumped frames, 70 shared a camera and 44 shared camera AND draw set, and those
    44 were byte-identical. **The filter also told me what the harness cannot reach**:
    every admissible frame was under 1,800 draws, so the outdoor era contributed zero
    comparable pairs and the null said nothing about it. Cheap rule: emit a per-frame
    content and camera fingerprint from the renderer, then make every cross-arm claim
    quote how many frames survived the filter. An A/B whose admissible n is not stated is
    not an A/B.
248. **A positive control has to be read with a statistic that can SEE the effect it is
    controlling for, and the obvious statistic usually cannot.** Part 25 poisoned a cube
    map dummy magenta to prove the cube sample reached the screen, then measured "what
    fraction of pixels are magenta" — requiring saturated red and blue. It read 0.24% and
    looked like a clean negative, which would have retired a real mechanism. But a cube
    sample arrives multiplied by a specular term: it TINTS a surface, it does not repaint
    it. Measured as "did the frame change at all against the same run with a white dummy",
    the same data says **80 of 110 frames differ, worst frame 72% of pixels and a max
    delta of 255.** The poison was working the whole time and the detector could not see
    it. **Rule: a control's readout should be the most sensitive difference you can
    compute — a per-pixel diff against the matched control run — not a semantic test for
    the marker colour you injected.** The marker is there to make the effect large, not to
    be recognised. Corollary for injected-colour instruments generally: always keep the
    unpoisoned run and diff against it, because "I can see my marker" is a much weaker
    question than "is this frame different".
249. **Run the NULL ARM FIRST, in the same block, and quote every effect as a multiple of
    it — a frame count is not an effect size.** Part 25's cube-map A/B reported "82 of 109
    frames differ" between the two arms and the positive control reported "80 of 110". Two
    counts that close read as two effects of the same size. They are not: **two runs of the
    SAME configuration differ on 82 of 109 frames**, because a synthetic-input recipe
    drifts (gotcha 75), so the count measures drift and nothing else. Only the magnitude
    against that floor separates them — median mean |RGB| of 0.069 for drift, 0.401 for the
    positive control (6x), and 0.038-0.085 for the real change (at or below the floor).
    This is the third form of one error in a single session: gotcha 246 was a count with no
    denominator, 248 was a control read with a statistic that could not see it, and this is
    an effect quoted with no null. **The mechanical fix covers all three: before measuring
    an arm, measure the arm against ITSELF, in the same block, on the same machine state —
    then no effect can be quoted except as a ratio.** `docs/measurement.md` had this rule
    for frame time already; it did not have it for pictures, and I did not transfer it.
250. **A function-local `static` clock is seeded on its FIRST CALL, not at process start —
    so the first line it stamps always reads zero, and a clock that reads zero whenever you
    look at it is worse than no clock because it looks like data.** Part 25 added elapsed
    seconds to the DebugJump log lines for exactly one purpose: a synthetic-input recipe has
    to place its menu presses after the jump lands, so "when did it land" is the number a
    recipe author needs. Written as `static const auto start = steady_clock::now();` inside
    the accessor, the first — and in a short run, only — line printed `at 0s`. Moving the
    epoch to namespace scope makes static initialisation seed it before `main`, which is
    process start to within milliseconds. **The general rule: an epoch belongs at the
    lifetime boundary you are measuring FROM, not at the first place you happen to read
    it.** Same shape as gotcha 151 in its quietest form — the instrument ran, printed, and
    reported nothing, and only comparing its number against another clock's (`CZ_FAKE_START_MS`
    was logging 16 s at the same moment) exposed it.
251. **A barrier that waits for an event only the thing it blocked could have caused is a
    deadlock, and it presents as a slow run rather than as a hang.** Part 25 added a
    WAITJUMP token to the synthetic-input arm so a recipe could say "navigate one interval
    AFTER the DebugJump screen opens" instead of "navigate at 136 seconds" — a fix for real
    fragility, since the jump landed at 131 s on one boot and the presses had already fired
    at 128 s. The first version froze the sequence and emitted NOTHING while waiting. It
    parked at 24 s and sat there for six minutes, because the frontend transition manager
    is only captured when the title CHANGES SCREEN, and the title only changes screen when
    something presses a button. The evidence had been on screen an hour earlier and was
    read as coincidence: the manager appeared three seconds after a DOWN press, i.e. that
    press caused it. **Before writing a wait, name the thing that will make the condition
    true and check it is not on the far side of the wait.** The fix here is the general
    one: a barrier REPEATS the preceding action while waiting, so `START,WAITJUMP,DOWN`
    means "press START until the screen lands, then navigate" — which is what a human does.
    Two details that fall out: the repeat must tap on the REAL clock, because the sequence
    clock is frozen by definition while parked and a frozen phase sticks the tap
    permanently on or off; and any one-shot edge in the repeated entry must be suppressed
    so it cannot re-fire every interval.
252. **A debug AI that exposes a "state" field may CHANGE IT UNDERNEATH YOU, and the
    symptom is every setting behaving identically.** Case Zero's AutoChuck takes a state at
    offset +0x70; part 25 set ITEM PICKER, EXPLORER and MISSION MASTER in turn and all
    three did the same thing — walk to the objective and wait — so the natural conclusion
    was that the write was landing in the wrong place or the label table was off by one
    (gotcha 241's shape). It was neither. A `process_vm_readv` of the LIVE process found 4
    (MISSION MASTER) after we had written 1 (ITEM PICKER) twice, and we never write 4: the
    AI promotes itself. **Three separate explanations were argued from plausibility before
    anyone read the field back** — including two of mine that were confidently wrong.
    Reading a written field back out of the running process is ~30 lines of ctypes here and
    it settles in one shot what argument cannot. The fix is to re-assert and COUNT: three
    overrides across 8,657 frames says the AI decides once, where thousands would have said
    the field is not the right lever at all, and only the counter distinguishes them.
253. **The button that OPENS a screen is not the button that closes it, and "press it
    again" is a guess dressed as symmetry.** Part 25 auto-closed a map screen by injecting
    BACK, because BACK is what the map is bound to on a 360 pad. BACK *opens* it; the close
    is B. The injection worked perfectly and the screen never moved, which reads exactly
    like "the press is not reaching the guest" and sent the next round of debugging at the
    input path. **When an injected input has no effect, separate "it did not arrive" from
    "it arrived and meant something else" before touching the delivery path** — a counter
    at the delivery point answers the first in one run. The operator knew the right button
    immediately; a question would have been cheaper than the experiment.
254. **An admissibility filter built on EXACT equality is a test for stasis, and on an
    animated scene its n is zero by construction — the fix is a different statistic, not a
    better recipe.** Two arms of a picture A/B are comparable only where they rendered the
    same thing, which this project spells as "the draw fingerprint and the camera
    fingerprint agree" (gotcha 247). Part 25 measured 13-44 surviving frames of ~300 on the
    old input recipes, every one under 1,800 draws, and concluded the RECIPE could not
    reach the outdoor era admissibly. Part 26 built a route that lands in a crowd at 7,300
    draws and re-measured with two runs of ONE configuration: **422 of 13,056 frames match,
    none of them above 141 draws, and ZERO of the 12,174 outdoor frames match at all** —
    the same answer, on a route that demonstrably goes where it was supposed to. The route
    was never the problem. **A crowd of animated actors does not produce a bit-identical
    draw list twice**, so exact equality selects for the frames where nothing is happening;
    `frame_compare.py`'s docstring records the same failure from the other end, where 257
    "perfectly aligned" frames turned out to be 257 copies of an empty scene. The
    diagnostic that separates "the arms disagree" from "the filter cannot be satisfied" is
    the NULL: run the filter on two runs of one configuration first, and if it reports
    nothing there, it can never report anything. What replaces it is an ERA AGGREGATE with
    its noise floor measured from that same null pair — here, over 12,000 frames above
    1,800 draws, medians of mean-luma and distinct-colour count that reproduce to 0.94% and
    0.76%. Aggregate over the era, never align within it (gotcha 38); what was missing was
    a measured null for the era, and one null pair supplies it.
255. **Name your API objects before you chase a message that names one.** For eight parts
    this project ran without the Vulkan validation layer, and for one part with it: of the
    five defects it reported, four were identifiable by reading the code and the fifth said
    only `VkImage 0x2350000000235`. This renderer creates images in five different places,
    so the handle was not a lead. Enabling `VK_EXT_debug_utils` alongside the layer and
    calling `vkSetDebugUtilsObjectNameEXT` at each creation site — ~40 lines, free when the
    layer is off — turned the next run's message into `[resolve snapshot 14A7A000 96x45
    slot 32]`, and the other thirteen into a halving chain that was recognisably one bloom
    pyramid. The defect was diagnosed from the names alone. **A validation layer tells you
    the rule that was broken; only you can tell it which of your objects broke it**, and
    the cost of teaching it is a few lines at each creation site.
256. **With a BINDLESS descriptor heap, "nothing indexes that slot yet" is an argument, not
    a guarantee — write the descriptor LAST.** Case Zero's resolve snapshots were created
    mid-frame: image, then descriptor, then a fill-and-transition recorded into the frame's
    own command buffer. The descriptor becomes visible to that whole command buffer at
    once, including every draw already recorded in it, so between the write and the
    transition there was a descriptor claiming `SHADER_READ_ONLY` on an image still in
    `UNDEFINED` — undefined CONTENT for anything that indexed it. Nothing did, because a
    draw can only learn that slot number from a lookup that would have missed; the layer
    reported it 14 times anyway, and the layer is right to, because with descriptor
    indexing neither it nor you can prove which slots a shader reads. The fix is ordering:
    transition in an immediate submit, THEN publish the descriptor — which the snapshot
    VIEW path in the same file already did, which is exactly why views never appeared in
    the messages. When one path in a file is quiet and its twin is not, read the quiet one.
257. **A FETCH COUNT IS NOT A SCREEN AREA, and quoting one as if it were the other will
    make a measured null look like a broken instrument.** Case Zero's `06805000` is an
    environment map the title renders itself and it is **35.9% of every cube-map fetch** in
    an outdoor run — the number part 26 built the whole cube snapshot path on, and rightly.
    But when the map is declined to a white dummy, the frame's era-median luma does not
    move outside the run-to-run spread, while declining EVERY cube map moves it eight times
    the spread. Both facts are true: the sampler is asked for that map constantly, and the
    surfaces asking are scattered reflective patches covering very little of the screen.
    **A fetch count measures how often a thing is ASKED FOR; a median over pixels measures
    how much of the picture MOVED**, and nothing converts between them. The failure mode is
    the reasoning that follows: a large fetch share plus an invisible aggregate reads as
    "my instrument is blind", which sends the next session to rebuild the harness instead of
    to the operator. The way out is a calibration arm that changes the same subsystem
    WHOLESALE — here, "no cube map at all" — because an instrument that resolves the class
    but not the member has told you the effect is small, not that it cannot see.
258. **RUN THE THIRD BASELINE BEFORE PUBLISHING A MULTIPLE OF A TWO-RUN NULL.** Part 26's
    outdoor cube A/B, on two baseline runs, put one arm at **12.0x the null** on median
    distinct-colour count — a publishable-looking result with the null measured in the same
    block, arms alternated, everything this project's rules ask for. The third baseline
    landed **5.4% away from the other two on that same statistic, whose two-run null had
    read 0.12%**, and the 12x collapsed into the noise. Two runs give one difference, and
    one difference cannot distinguish a tight statistic from a bimodal one that happened to
    land twice on the same side (gotcha 159's shape, in the picture domain rather than the
    timing one). The tell was available beforehand and was ignored: two independent pairs
    had already measured that statistic's null at 0.76% and 0.12%, a 6x disagreement, which
    is the null saying it is not a null yet. **A statistic earns "usable" by reproducing
    across three runs, not by producing a small number once** — here median mean-luma did
    (0.55% over three) and median distinct-colour count did not.
259. **A SINGLE-FRAME GPU TRACE IS SELF-CONTAINED, AND THAT MAKES IT A BETTER ORACLE THAN A
    CONTINUOUS STREAM.** Case Zero's round-1 captures were `trace_gpu_stream` — one huge
    `.xtr` from boot, 1.6 GiB for the boot alone, and a 2 GiB cliff that had to be fixed at
    source. Round 2 asked for "the same method" and the operator deviated deliberately,
    taking seven single-frame F4 traces instead. That is strictly better for any question
    about a PLACE: each file opens with an `EdramSnapshot` and then carries a `MemoryRead`
    with the actual sampled bytes of every texture, vertex and index buffer the frame
    touched, so it replays standalone — a texture can be reconstructed without seeking into
    a stream, each file pairs unambiguously with the spot it was taken at, and 60-75 MB
    replaces gigabytes. **Ask for per-place single frames unless the question is genuinely
    about a sequence.** The general form: an artifact that carries its own starting state
    is worth far more than a larger one that has to be replayed from the beginning.
260. **NAME THE SAME OBJECT THE SAME WAY ON BOTH SIDES OF AN ORACLE, AND THE COMPARISON
    STOPS BEING GUESSWORK.** Comparing our renderer against a capture looked hard because
    the obvious keys do not survive: texture ADDRESSES are per-session allocations (a scan
    of 186,398 draws of the round-1 gameplay capture found none of the addresses our runtime
    reports for the same material), and draw indices differ because the two stacks do not
    issue identical work. What DOES survive is a content hash of the shader microcode —
    which this runtime already computes at `IM_LOAD` and which the offline translation
    pipeline computes for the cache. Naming shaders that way in the capture reader made one
    draw identifiable in both stacks in a single step: same hash, same vertex count, same
    bindings. **Before building a comparison, find the identifier that is a property of the
    CONTENT rather than of the session.**
261. **A DUMP FROM ANOTHER TOOL CAN BE BYTE-SWAPPED, AND THE SELF-TEST IS WHAT TELLS YOU SO.**
    Xenia's `dump_shaders` writes `.ucode.bin` dword-swapped relative to the guest's
    big-endian bytes. Hashing them directly made all 357 shaders in a capture look NEW
    against a 410-shader cache of the same game — a "we are missing the entire world"
    result that would have justified a large piece of work. The thing that caught it in one
    step was hashing OUR OWN dumps first and checking they reproduce their own filenames:
    410 of 410 did, so the function was right and the difference had to be in the data.
    **When a comparison says everything differs, verify the comparator against data whose
    answer you already know before believing it.**

262. **A REPLAY THAT IGNORES A PACKET TYPE REPORTS ABSENCE, AND THE TITLE'S DOMINANT PATH
    IS NOT ALWAYS THE OBVIOUS ONE.** Three of this project's `.xtr` tools decoded
    `SET_CONSTANT` and `SET_CONSTANT2` and silently dropped `LOAD_ALU_CONSTANT` (opcode
    0x2F), which loads a constant block out of guest MEMORY rather than carrying it in the
    packet. In `w1_spawn.xtr` the title issues **620 `LOAD_ALU_CONSTANT` against 36
    `SET_CONSTANT`** — so the tools reconstructed almost none of the ALU constant file and
    every constant they printed for hardware was a zero that meant nothing. The runtime's
    own `pm4.cpp` had handled 0x2F since phase 4; the oracle tools were written later and
    from the same mental model, not from the same code. **Census the opcodes a capture
    actually contains before trusting a replay of it** — it is four lines and it is the
    only thing that distinguishes "the guest never wrote that" from "we never read it".

263. **A CAPTURE IS NOT OMNISCIENT, AND AN UNRECONSTRUCTIBLE REGISTER MUST SAY SO RATHER
    THAN KEEP ITS STALE VALUE.** With 0x2F handled, **81 of `w1_spawn`'s 620 constant
    loads read an address the trace never recorded** — the title cycles its constant
    buffers through many addresses and Xenia records only the ranges it saw sampled. The
    natural implementation leaves the previous value in the register, which prints a
    plausible number that is some earlier draw's leftover and reads as hardware's answer.
    **What exposed it was an IMPOSSIBLE value, not a suspicious one**: the ground pixel
    shader uses `c255.w` as its literal 1.0 (`min r0.x, r1.x, c255.w`, `subsc r1.z,
    c255.w, r1.x`) and the replay said c255 was `(0,0,0,0)`, a value the shader could not
    have run with. The fix is to mark the range UNKNOWN on a load whose source bytes are
    missing and print `UNRECOVERABLE`. **Ask of every value an oracle gives you: could the
    guest have run with this?** — a stale-value bug that lands on a plausible number is
    invisible, and only the impossible one announces the whole class.

264. **A FILTER THAT SELECTS ON THE PROPERTY UNDER TEST CANNOT FIND A VIOLATION OF IT.**
    Part 26 reported "414 of 414 cube-declared draws on hardware read stack depth 5 and
    dimension 3 — no disagreements at all" and concluded that our ~14,670 declined cube
    fetches a run are a disagreement we manufacture. The conclusion was right; the
    measurement could not have produced any other answer. It selected draws where a
    cube-declaring shader was bound and then counted the fetch constants that ALREADY read
    cube — and a disagreeing slot reads 2D, so it was outside the population by
    construction. The question has to be asked per DECLARED FETCH SLOT, from the shader's
    own sidecar, exactly as the runtime asks it (`tools/xtr_cube_agreement.py`): 0 of
    13,203. Same answer, and now it is one a defect could have failed.
    **Before quoting an N-of-N agreement, write down what the disagreeing case would look
    like and check your filter would have admitted it.** This is gotcha 25 with a
    denominator instead of a grep.

265. **THE SAME MESH DRAWN TWICE IN A FRAME WITH THE SAME STATE IS THIS TITLE'S TILING,
    NOT TWO PASSES.** Item 00f's last surviving lead for the white ground was that its
    mesh is drawn twice with `mask=F`, same vertex shader and same textures, and that one
    of the two might be meant to combine with the other rather than overwrite it.
    `CZ_VK_DRAW_PROBE` prints the scissor, and the two draws carry `scissor 0,0 640x720`
    and `scissor 640,0 640x720` — the left and right 640-wide halves this title renders in
    (the CLAUDE.md note about tiles, arrived at from the other end). **A per-draw census
    that does not print the SCISSOR makes every tiled title look like it double-draws**,
    and the duplicate is the single most inviting wrong lead a census can offer.

266. **A "DEBUG FLAG THE SHIPPED BUILD LEFT UNSET" IS OFTEN A KILL SWITCH THE SHIPPED
     BUILD TURNED ON — AND THE DIFFERENCE IS ONE SCAN.** Gotcha 215 recorded this
     engine's 640-caller log sink and explained its silence as "a debug byte per
     category that a shipped build leaves at zero", making the fix sound like a hunt
     across hundreds of independent flags. That framing survived unexamined for
     thirteen parts and it was wrong twice over. A scan of `.text` for `lis`-resolved
     byte references finds **one** address, `0x829EC974`, read by **2,013 sites and
     written by none**, every site the identical shape — and the image ships that byte
     as **1**, with the branch `bne <skip>`. So the polarity is inverted from the
     guess: the layer is not waiting to be switched on, it was switched OFF, and
     clearing one byte re-enables all 2,013. `CZ_GUEST_LOG=1` alone printed **0 lines**
     over an 11,168-line run; with the byte cleared the same route printed **1,239**,
     which is the null and the positive control of the same measurement.
     **The transferable part is the method, and it is cheap: for any global you suspect
     gates diagnostics, count its READERS and its WRITERS separately.** A byte with
     thousands of readers and zero writers is a build-time constant, and its value in
     the image tells you the polarity without reading a single call site. Reasoning
     about which flags a release build "would have" left unset is guessing at a fact
     the binary states outright.
     **Two bytes, not one.** Many of the 2,013 are the assert formatter, whose path
     continues into a `twui` trap guarded by a SECOND byte (`0x82AC3EAD`, 592 readers,
     2 writers). Clear the log gate alone and previously-silent asserts become fatal.
     Setting the trap byte is not hiding anything — the assert still prints, with its
     file and line; suppressing the trap is what makes the message reachable at all.

267. **AN ORACLE CAN ONLY SETTLE A DEFECT SUBTLER THAN THE LOUDEST DEFECT ON YOUR OWN
     ARM.** Asked whether this port's LOD pops in too late, the operator did the right
     thing and looked at the same distance in Xenia — and reported that it does not
     obviously change there, *but that hardware's transitions are far less visible because
     hardware's textures are not broken.* That second clause is the whole finding. An LOD
     swap is perceived as a change in SURFACE DETAIL, and our arm's open white-patch defect
     replaces surface detail on world geometry with a flat `rgb(180,180,180)`. So a swap
     hardware makes invisibly reads as loud on ours, and one hardware makes visibly is
     indistinguishable from the plateau: the comparison cannot return a wrong answer,
     it cannot return an answer at all. **A confounded oracle reading is not a null
     result**, and filing it as one ("we checked, Xenia looks the same") converts an
     unanswered question into a closed one — the most expensive kind of documentation
     error, because nothing downstream ever re-opens it.
     **The rule: before asking the oracle, name the loudest UNFIXED defect on the channel
     you are about to read, and check the effect you are hunting is larger than it.** If it
     is not, the item is PARKED with a stated unblock condition, not refuted. This is the
     A/B admissibility rule (CLAUDE.md) reached from the picture side rather than the
     draw-set side, and the same discipline as gotcha 172 — a retirement is only as good as
     the oracle it was measured on, and here the oracle is fine while OUR arm is the
     contaminated one. It also outranks acting on the result: do not build the candidate
     fix for a defect an unusable comparison failed to confirm.

267. **A GUEST STRUCTURE HANDED TO A DMA DEVICE HOLDS PHYSICAL ADDRESSES, AND IN A FLAT
     RECOMPILER MAP THOSE ARE NOT THE ADDRESSES THE CPU USES.** This silenced Case Zero
     for twenty-eight parts and it will silence Case West unless someone checks.

     The 360's XMA decoder is a DMA device: the title writes PHYSICAL addresses into the
     hardware's context structure, and on the console the MMU makes them the same bytes
     the CPU reaches through a cached virtual alias. A static recompiler's address space
     is one flat 4 GB map in which the physical arena is a WINDOW — here at 0xA0000000 —
     so the physical address and its virtual alias are two different offsets into `base`
     and *nothing aliases them*. Read the structure's pointer literally and you get a
     page of zeros.

     The failure mode is what makes it expensive: **a zero page decodes to silence, not to
     an error.** Every layer above looks healthy — the pump runs at the right cadence, the
     mixer submits its frames, the decoder accepts its packets — and the symptom is
     indistinguishable from "the game produced no audio", which is where three sessions of
     this project's audio work were aimed.

     What found it was one field on one log line. The trace said

         NtReadFile('game:\data\audio\music.big', 131072 bytes @ 16666624)
              -> 131072 into A2538000
         [xma] ctx0 in0=02538000 64 pkts (131072 bytes): 0 non-zero (0.00%)

     and the two addresses are 0xA0000000 apart. **A read that reports the right byte
     count into the wrong place is indistinguishable from a correct one unless the trace
     prints its DESTINATION** — that is the transferable half, and it is one `%08X` on
     every file-IO trace you will ever write.

     Corroboration was free and worth taking: 16,666,624 is exactly the offset of
     `PressStartPrologue.xma` in `music.big`, and 131,072 is exactly the 64 packets the
     context declares. When the guest's numbers line up that precisely with an asset, the
     guest is not the one that is wrong.

     **The general rule: for any structure the guest fills in for HARDWARE rather than for
     itself — audio contexts, GPU ring buffers, DMA descriptors, command lists — ask which
     address space its pointers are in before dereferencing one.** The kernel usually
     already states the convention in the opposite direction; here
     `MmGetPhysicalAddress_x` was three lines long and had implemented the exact inverse
     (`address >= 0xA0000000 ? address & 0x1FFFFFFF : address`) since phase 1.

268. **A STUB THAT FAKES A STATE MACHINE REFUTES LESS THAN A REAL IMPLEMENTATION, AND IT
     CAN RETIRE A TRUE HYPOTHESIS.** `CZ_XMA_NULL_DECODER` is a decoder that consumes its
     input and produces nothing, built so voices could start and stop without one. Part 16
     ran all three of its configurations against the prologue's frozen cinematic — always
     playing, never playing, and plays-then-ends with 19 start / 18 stop edges — got a
     frame-for-frame identical freeze in each, and recorded the hypothesis as **"refuted,
     not merely unconfirmed"**. It was the strongest form of negative result this project
     knows how to produce, and it was wrong: with a REAL decoder the freeze disappears
     (10,527 frozen frames -> 159, 15% of frames covered -> 99.94%).

     The arm moved the *predicate the title polls* — the input-buffer-valid bits. It could
     not move what the cinematic was actually waiting on, which is downstream of PCM
     existing. **An arm refutes a hypothesis only over the states it can actually reach**,
     and a null implementation reaches the states its author thought were load-bearing.
     Both polarities of a predicate is not the same as the whole mechanism.

     So: when a null/fake arm returns a negative, write down *what it cannot do* next to
     the conclusion, and re-ask the question when the real thing lands (gotcha 172, whose
     scope this widens — a retirement is only as good as the oracle it was measured on,
     and your own stub is an oracle). The cheap tell here was available at the time: the
     arm's own documentation said it "fabricates progress the real hardware would only
     make after actually decoding the audio", which names the gap exactly.

269. **A PROBE THAT REPORTS FROM INSIDE THE FUNCTION IT COUNTS GOES SILENT EXACTLY WHEN
     THE INTERESTING THING HAPPENS.** The cheapest way to add a counter to a guest
     function is to hook it, tick a 5-second clock at the end of the hook, and print
     when the clock fires. That is what `CZ_CINE_PROBE` did, and it is wrong in one
     specific and very likely case: *the function stops being called*. The counter then
     stops reporting, and "no output" is ambiguous between "not called", "called but no
     report due yet", and "the instrument is broken" — which is gotcha 151's blind spot
     wearing a different hat, because the arm has a counter and still cannot say what
     happened.

     Here it happened on the first run: `sub_824A0FC0` was entered ten times and then
     never again, which was THE ANSWER, and the probe's way of expressing that answer
     was to fall silent. It stayed readable only because the frame counter was visibly
     advancing in another instrument at the same moment, so "no report" could be read as
     "not called". That is luck, not design.

     **Drive a probe's reporting from a clock that runs regardless of the thing being
     measured** — in this runtime that is the graphics pump, which ticks whatever the
     guest is doing. Then "called 0 times in the last 5 s" is a printed line rather than
     an absence, and absence goes back to meaning the instrument is off.

     The general form: an instrument must not share a liveness dependency with its
     subject. It applies to any hook-and-report counter, any per-frame statistic printed
     from the frame path, and any log line emitted by the subsystem it describes.
