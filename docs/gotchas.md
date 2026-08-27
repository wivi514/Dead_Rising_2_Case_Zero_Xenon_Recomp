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

270. **WHEN TWO COMPONENTS YOU BUILT AGREE WITH EACH OTHER, YOU HAVE MEASURED YOUR OWN
     CONSISTENCY, NOT THE GROUND TRUTH. AN ORACLE HAS TO BE SOMETHING YOU DID NOT
     WRITE.**

     Part 29 established that the prologue cinematic's clock is driven by a PID tracking
     an audio stream position, and that the position freezes at 4.906667 s. Chasing why,
     it found that our own XMA decoder had produced 235,968 sample-frames for that voice
     while the guest's `SamplesPlayed` had pinned at 235,520 — **448 frames apart, out of
     a quarter of a million.** That agreement is genuinely informative: it says the voice
     played everything we handed it and nothing was lost between the two.

     It was then read as "so the clip ended", and recorded as a finding. It is 1.6% of
     the clip. The asset is `39694.xma`, 24,377,344 bytes, and summing the `frame_count`
     field of its 11,903 XMA2 packet headers gives **316.5 seconds** — which the
     operator's stopwatch (~5 min 10 s) then confirmed to within six seconds. The guest
     reads 262,144 of those bytes, once, and never asks for more, while `music.big`
     double-buffers correctly for the whole session on the same machinery.

     Both numbers in the agreement were correct. Both came from code in this repository —
     our decoder, and our reading of the guest's state through our own probe. Neither had
     ever been compared against the thing being described. **Two of your own components
     agreeing is a consistency check; it cannot be an oracle, however tight the agreement
     and however many digits match.** The tighter the agreement, the more persuasive the
     wrong conclusion.

     What makes this worth a number rather than an apology: **the discriminator had
     already been identified and written into the hand-off** — "whether 4.91 s is the
     clip's true length or where our decode stops has two opposite fixes, and
     `CZ_FILE_TRACE=1` plus `tools/big_list.py` tells them apart". The failure was not
     missing the question. It was **recording the likelier branch as the finding instead
     of leaving the item open until the third party answered**, when answering it cost
     one environment variable and one command.

     The rule, and it composes with 172 ("an untrusted path is not an oracle") and 268
     ("your own stub is an oracle, and it refutes less than you think"): before writing
     down a conclusion about what the guest's data *is*, name the third party that could
     refute it — the asset on disc, a Xenia trace, the operator — and ask whether you
     have actually asked it. If the answer is "no, but my two measurements agree", you
     have a hypothesis, not a finding.

271. **A FORMAT FIELD THAT IS ZERO IN EVERY ASSET YOU HAVE PLAYED IS NOT A FIELD YOU HAVE
     IMPLEMENTED. AND AN UNEXPLAINED STRUCTURAL ODDITY IN YOUR SUBJECT IS THE BUG,
     WAITING.**

     The XMA2 packet header carries `packet_skip` — how many packets to step over to
     reach the next packet **of the same stream**. This port's decode walk advanced by
     one packet, and that was correct for every asset it had ever decoded: music, sound
     effects and one-shot voice lines are mono or stereo, single-stream, and `packet_skip`
     is zero in all of them. The walk was byte-for-byte right for 29 parts.

     Then something played 5.1. The 360 decodes six channels as several **interleaved
     2-channel streams sharing one packet stream**, one XMA context per pair. Walking `+1`
     made every context in the group decode the other streams' packets as its own —
     2.66x too much audio, measured — which filled each output ring about three times
     faster than the title's mixer drained it, wedged the whole voice group after a single
     buffer, and presented as a cinematic that ping-ponged forever. Four sessions of
     candidate explanations sat on top of that.

     **The general form:** when you transcribe a hardware format, the fields that are
     constant across your corpus are the ones you have not tested, and a corpus of "every
     asset the port has played so far" is exactly the corpus that hides them. Grep the
     asset for the field's distribution before believing the simple case is the only case
     — here `packet_skip` reads 0/1/2/3 across 11,903 packets, which is thirty seconds of
     Python and would have said "this is interleaved" outright.

     **The second half is sharper, and it is about attention rather than coverage.** The
     fingerprint of this bug was on screen twice, in writing, before it was found:
     contexts 5, 6 and 7 all reported the *same* input buffer address. It was recorded
     both times as "worth a look, not worth a conclusion" and left. Three decoders on one
     buffer has exactly one sensible explanation, and it is the one that names the defect.
     **An unexplained structural oddity in the thing you are debugging is a lead with a
     shelf life, and when it recurs it has stopped being an oddity.** Write down what
     would explain it, not just that it is odd.

     Composes with 270: the operator's one sentence ("the clip is around 5 min 10 s")
     refuted a wrong conclusion and, once the asset was finally read, the packet headers
     gave both the true duration and the interleaving in the same pass.

272. **A PROCESS-WIDE POLICY SET BY ONE SUBSYSTEM STOPS HOLDING THE MOMENT A SECOND
     SUBSYSTEM GAINS ITS OWN ENTRY POINT TO THE SAME LIBRARY — and the failure is silent
     because the policy has no observer.** This port tells SDL not to install signal
     handlers, so that `timeout N ./cz_runtime` — every gate run here — terminates the
     process whatever the runtime is doing. The hint sat next to `SDL_Init` in the window
     module, which was correct for as long as the window was the only thing that touched
     SDL. Phase A/V then gave the audio device its own `SDL_InitSubSystem(SDL_INIT_AUDIO)`
     call, reached from a guest thread, on a path where the window module had already
     returned early (`CZ_NO_WINDOW=1`). From that commit on, **every headless run with
     sound ignored `timeout`**: measured as exit 124 at 20 s with `CZ_NO_AUDIO_OUT=1` and
     still alive at 180 s without it, same binary, one variable.

     **What makes this class expensive is that the symptom is a LONGER SUCCESSFUL RUN.**
     There is no error, no log line and no non-zero exit — a 420 s recipe simply produces
     ten minutes of frames, so every per-run statistic taken from it silently covers more
     wall time than the recipe says, and a comparison between an audio arm and a
     no-audio arm compares different durations while looking perfectly matched. It was
     found only because a human noticed an A/B block was not advancing.

     **The rule:** initialisation-order policy belongs at the first point in the process
     guaranteed to run before any use of the library, not next to the first use anyone
     wrote. And when a subsystem acquires a second entry point to something global, the
     question to ask is not "does the new path work" but "which invariants did the old
     path own that nobody re-established". Compose with 5: a policy with no counter
     cannot be shown to be in force, and this one had none for two parts.

273. **A THRESHOLD PROBE CANNOT TELL "BELOW" FROM "EQUAL", AND A CURVE'S FLAT SPOT MAKES
     A VARYING INPUT LOOK LIKE A CONSTANT.** Two ways the same tone-map plateau misled
     this project for three parts, both worth carrying because both look like results.

     `XE_FLOOR_PAINT` asked, per pixel, whether a `max(expr, K)` took its floor, and
     reported zero. That was read as "therefore `expr > K`, therefore the colour is
     pinned". But with the constants finally read off hardware, `expr = 0.25x + 0.75` and
     `K = 1.0`, so `expr < K` requires `x < 1` — and the surfaces under test sit at
     `x = 1`, where `expr == K` exactly. **The probe returned the same answer for the
     hypothesis and its negation.** Before believing a threshold probe, substitute the
     constants and check that both outcomes are reachable for the population you pointed
     it at (compose with 264, and with 30).

     The second half: the same shader's output is `sqrt` of a curve whose derivative
     vanishes at `x = 1`, so a **10% spread in the input quantises to one 8-bit value**.
     52,840 pixels at exactly `rgb(180,180,180)` was recorded as "a plateau, not a bright
     surface — these are not shaded at all", and it supports no such thing: a normally
     shaded surface sitting at full exposure produces exactly that picture. **Where a
     transfer function is flat, an output histogram measures the function, not the
     input.** Differentiate the curve before reading a spike in its output as a constant
     in its input.

274. **AN ORACLE THAT CANNOT ANSWER IS A FACT ABOUT THAT MEMBER, NOT ABOUT THE
     POPULATION.** Part 27 asked one of seven single-frame captures for the pixel-shader
     constants behind the white-surface defect, got `UNRECOVERABLE` for the three
     registers that decide the whole tone curve (the loads read memory the trace does not
     carry — gotcha 263), and recorded the conclusion as *a capture that carries the
     constant-buffer memory would close the input list*, i.e. as a request for new ground
     truth. The same question asked of the other six captures answers on **five** of
     them, from data already on disk for weeks. Sixty-eight draws, identical values, and
     the answer retired an eight-step chain of inference.

     **The rule is one line: when an oracle says "I cannot tell you", ask the other
     members before asking for a new oracle.** A capture set, a log set, a dump directory
     and a shader bank are all populations, and this project has now made the same
     mistake from three directions — 3 (a scanner's zero), 264 (a filter that selects on
     the property under test) and this one. New ground truth is the most expensive thing
     available here and it was very nearly requested to answer a question five files
     already answered.

275. **A RESOLVE DESTINATION CAN NAME A SUB-REGION BY ITS ADDRESS INSTEAD OF BY ITS
     SCISSOR, AND A RENDERER THAT ONLY UNDERSTANDS ONE OF THE TWO LOSES THE OTHER
     SILENTLY.** Case Zero packs four 1024x1024 shadow cascades into one 4096x1024 atlas.
     Each resolves a 1024x1024 region **with the window scissor at the origin**, and the
     four are told apart only by `RB_COPY_DEST_BASE` being pre-offset by 0x20000 — which
     in Xenos tiled address space is exactly +1024 texels in X (a 32bpp macro tile is
     32x32 texels = 4096 bytes, a 4096-wide surface is 128 tiles per tile row, so +32
     tiles = 0x20000). This renderer already un-offset the SCISSOR form of the idiom, for
     the frame's two 640-wide tiles, and derived the offset FROM the scissor — so for the
     cascades the subtraction was a no-op, the four became four disjoint snapshots, and a
     fetch of the base address read zero past column 1023. **86.7% of our shadow atlas was
     empty where hardware's, dumped from the same capture, is 3.5% empty.** The failure is
     silent in the worst way: the snapshot is the right size, it is populated, and it is
     served — it is simply a quarter of the picture, and the consumer has no way to say so.
     The general form: **when a guest can express "part of a bigger surface" two ways,
     implementing one of them makes the other look like a surface of its own.** Ask which
     ways a destination can carry an offset before concluding a map is half empty for a
     reason inside the map.
     The other half of this is that it was CHECKABLE all along and the check took ten
     minutes: the capture carries the consumer's copy of the atlas as a `MemoryRead`, and
     `xtr_resolve_census.py` prints the title's own resolve destinations and regions. A
     surface you RENDER is not automatically a surface you cannot compare (gotcha 172's
     rule, pointed the other way).

276. **WHEN THREE OR FOUR SUCCESSIVE ARMS ALL REPORT "UNMOVED", THE INSTRUMENT CLASS IS
     THE FINDING — STOP PERTURBING INPUTS AND CHANGE WHAT YOU ARE MEASURING.** Part 31
     zeroed the sun colour, zeroed an additive term, zeroed the entire multiplicative path
     of every shader reading a given constant (blacking out 61.5% of the frame), and
     quartered the exposure (halving the scene's mean luma) — and the pixels under
     investigation sat at exactly the same 8-bit value through all four. Each individual
     result reads as "not this one, try the next"; the four TOGETHER say something the
     individual results do not, which is that **the pixels are not downstream of any input
     to the pass being perturbed.** Four parts of this project were spent inside one
     instrument class. The tell is that every arm engages hugely and moves everything
     except the thing being investigated.
     Corollary on what to reach for instead: a whole-frame arm can only say "this input
     does not reach those pixels". Naming the DRAW needs a per-draw instrument — a census
     of one frame, or removing one draw and diffing — and no amount of refining the
     whole-frame arm converges on it.

277. **AN ARITHMETIC COINCIDENCE THAT FITS TO THREE DIGITS IS A HYPOTHESIS, AND IT KEEPS
     ITS STATUS UNTIL A MECHANISM IS MEASURED — NOT UNTIL A BETTER COINCIDENCE TURNS UP.**
     `180/255 = 0.70588` and `sqrt(0.5) = 0.70711`. That fit carried four parts of work.
     Part 27 read it as a gamma encode; part 30 retracted the gamma and re-derived the
     same 180 from the shader's own trailing `sqrt` and its own constants — a second,
     independent derivation, which felt like confirmation. Part 31 then showed the pixels
     are invariant under scaling the exposure that curve multiplies by, so **they are not
     outputs of that curve at all** and both derivations were explaining a number rather
     than a picture. Two independent derivations of the same value are still not evidence
     that the value came from there. The question that would have caught it four parts
     earlier is the cheap one: **what would move these pixels, and does it?**

278. **A SYMPTOM THAT SURVIVES AN ARM IS NOT A SYMPTOM THE ARM DOES NOT AFFECT — NAME THE
     PROPERTY THAT WOULD HAVE CHANGED BEFORE CONCLUDING NOTHING DID.** Part 31 fixed the
     shadow atlas, then read two screenshots of the CONTROL arm, saw the same qualitative
     symptom the operator had reported on the fixed build — a lit region that moves around
     the frame with the camera — and wrote that the fix was "neither the cause nor the
     cure". The operator's own fold-ON shots half an hour later said the opposite: *"much
     wider, the spot where shadows are is actually in front of the camera."* Both arms have
     a camera-dependent lit region, so "the symptom is present in both" was true and
     useless. What separated them was its **extent**, and the mechanism predicted exactly
     that: with only one of four cascades populated, a pixel past the first split distance
     (8 m, from the title's own `pc(46)`) samples an empty region and reads as occluded;
     with all four, the shadow term is real out to the third split at 32 m.
     **The discipline is to state, before looking, which measurable property the fix
     should move** — here a radius that falls straight out of constants already read — and
     to check that one rather than asking "does it still look wrong". A defect that is
     40% fixed looks exactly like a defect that is 0% fixed if the only question asked is
     whether it is gone.
     Two aggravating factors worth copying into any port's habits: the comparison was
     being made against a REMEMBERED pair of images because the arm's own screenshots had
     not been saved to disk yet (gotcha 50/51/86, and the reason the screenshot tooling
     got fixed the same afternoon), and two shots at two different cameras is enough to
     establish "a moving region exists here" and never enough to establish "the two arms
     are the same".

279. **AN ARM WHOSE FAILURE MODE IS THE SYMPTOM IT WAS BUILT TO RULE OUT WILL CONFIRM
     THE WRONG ANSWER, CONFIDENTLY.** Part 32 needed to separate "this geometry was never
     submitted" from "it was submitted and rejected by stale depth" for a shadow-cascade
     pass, and this runtime has an arm for exactly that: `CZ_VK_NO_DEPTH_TEST=1`, whose
     comment says it "draws everything regardless of depth". It came back with the depth
     surface **100% zero** — i.e. "nothing was submitted", which is the opposite of the
     truth. Vulkan ties depth WRITES to the depth TEST: with `depthTestEnable` false the
     attachment is not written at all, whatever `depthWriteEnable` says. On a colour pass
     that arm does what it claims; on a DEPTH-ONLY pass it empties the buffer, and an
     empty buffer is precisely the symptom being investigated.
     The replacement is one line — keep the test enabled and force the comparison to
     ALWAYS (`CZ_VK_DEPTH_ALWAYS=1`) — and it gave 46.875% zero -> 1.86%.
     **The transferable rule: before believing an arm, ask what it would print if the
     defect were absent AND what it would print if the arm itself were broken. If those
     two are the same string, the arm cannot answer.** This is gotcha 30's "a test that
     has never failed" from the other side: here the arm always "fails", and its failure
     is indistinguishable from a finding. The cheap guard is a positive control on the arm
     — run it somewhere the answer is already known — which is the same discipline
     `measure-the-arm-against-itself-first` states for magnitudes.

280. **A CAPTURE'S MEMORY RECORDS ARE SNAPSHOTS WITH A TIME, SO A CAPTURE CANNOT SUPPLY
     ANY SURFACE THE GPU PRODUCES INSIDE THE TRACED FRAME.** This corrects the second half
     of gotcha 275, which said a surface you RENDER is still comparable because the capture
     carries the consumer's copy of it. That is true only when the address is not itself a
     resolve destination in the same trace.
     Xenia's `.xtr` dumps the bytes behind a resource the first time the GPU reads it, and
     never again. Part 31 dumped this title's shadow atlas from `w1_spawn` at `1812F000`,
     found it 96.5% populated against our 13.3%, and quoted that gap for a whole part.
     There is exactly ONE memory chunk covering that address, taken at walk position 39;
     the first resolve INTO it is at walk position 3522. The bytes are what was there
     BEFORE the atlas was produced — and because the title reuses that address for the
     composited scene, what was there is a photograph of the previous frame. Detiled, the
     game's own HUD is legible in the "shadow map": *8 KILLED*.
     **A dense, plausible, wrong oracle is worse than none**, because it converts a
     real defect into a solved one. The gate is mechanical and costs nothing: while
     walking the trace, record every `RB_COPY_DEST_BASE` issued under `RB_MODECONTROL`
     edram_mode 6, and when dumping bytes for an address, refuse — exit non-zero — if
     every covering snapshot predates the first resolve to it.
     `tools/xtr_draw_bindings.py --dump-texture` does this now and prints *"a sound
     oracle"* for the ordinary case, which is what keeps the gate readable rather than a
     warning people learn to skip.
     The corollary for the port: when the only oracle for an intermediate surface is gone,
     the target to measure against is not another emulator's number but **the surface's
     own definition** — a shadow map's unwritten region must read FAR, so the yardstick is
     100% coverage, not somebody's 96.5%.

281. **`min`, `max` AND `saturate` LAUNDER NaN INTO A FINITE CONSTANT, SO A NaN DETECTOR
     DOWNSTREAM OF THEM READS CLEAN — PUT IT AT THE OPERANDS.** On host GPUs
     `max(NaN, K)` returns `K` and `saturate(NaN)` returns 0, so any expression of the
     shape `(max(x, K1) - saturate(K1 - x)^2) * K2` maps EVERY NaN input to the same
     finite output — for Case Zero's shared tone epilogue, exactly
     `sqrt(0.5) = rgb(180,180,180)`, the white-surface plateau. Part 27's `XE_NAN_PAINT`
     tested `isnan(oC0)` at the end of the shader, got zero magenta, and the result was
     recorded as "the value never was a NaN"; the value was a NaN, and the detector was
     downstream of the laundering it was meant to catch. Moving the same test to the
     max's OPERANDS (`XE_FLOOR_IS_NAN`) painted every plateau pixel in one run.
     The corollary that makes such a NaN findable at all: a laundered NaN is INVARIANT
     UNDER EVERY UPSTREAM CONSTANT — four whole-frame arms (sun, additive, multiplicative,
     exposure) all reading "unmoved" is not four eliminated candidates, it is the NaN
     signature. And a comparison predicate cannot see one either: `NaN > K` and
     `NaN < K` are both false, so a flag defined as `(b > a)` reports the SAME thing for
     "the floor was not taken" and "the operand was NaN" — which is also what makes the
     predicate swap its own control (same population, opposite colours).

282. **RUN THE VULKAN VALIDATION LAYER BEFORE THEORISING ABOUT A PICTURE DEFECT — A
     PIPELINE TYPE MISMATCH IS UNDEFINED VALUES THAT PRESENT AS A SHADING BUG.** Binding
     a `R32_UINT` vertex attribute against a `float4` shader input violates
     VUID-VkGraphicsPipelineCreateInfo-Input-08733; nothing fails, and what the driver
     delivers is the raw bits reinterpreted as float — NaN whenever bits 30..23 are all
     ones, plausible garbage otherwise, per vertex, so the symptom is hard-edged patches
     on correctly-shaped geometry. Case Zero spent parts 26-31 measuring that symptom
     with picture instruments while one `CZ_VK_VALIDATION=1` run would have printed the
     defective pipelines by location. The mismatch class to expect from a recompiler: an
     emitter that types inputs by USAGE (TexCoord = float4) meeting a runtime that
     formats attributes by FETCH FORMAT (packed normal = raw uint) — two correct tables,
     joined on the wrong key. This title wraps packed 10_11_11 normals as TEXCOORD;
     Fable 2 wraps them as NORMAL (uint4), which is why the combination was never seen
     before Case Zero.

283. **ROBUSTNESS BOUNDS THE BUFFER, NOT YOUR SUB-ALLOCATION.** When every stream is
     sub-allocated from one arena VkBuffer, `robustBufferAccess` clamps reads to the
     ARENA — an index that runs past its own stream but lands inside the arena is exactly
     as wrong as without robustness, and a "no change under the robust arm" result says
     nothing about per-stream overruns (gotcha 279's shape: the arm's blind spot prints
     the same string as the defect's absence). Vertex bindings carry no size in plain
     `vkCmdBindVertexBuffers`. The instrument that CAN answer is a CPU-side census that
     walks the index VALUES against each stream's declared size — and note the standing
     guard's trap: bounding `indxOffset + indexCount` bounds the number of indices, not
     the vertices they name.

284. **AN END-OF-RUN REPORT DOES NOT SURVIVE THE STANDARD RECIPE'S `timeout` KILL — prove
     an arm engaged DIFFERENTIALLY, or the engagement check reads zero on every gate
     run.** Case Zero's renderer dumps its `g_stats` counters at clean shutdown; every
     headless recipe ends in `timeout`, which kills the process before the dump, so
     `grep "window Y also scaled"` returned 0 on a run whose arm demonstrably engaged.
     The counter was fine, the exit path never ran — the same blind spot as gotcha 151
     (an arm with no counter cannot be shown to have engaged) wearing the opposite
     disguise: an arm WITH a counter that is never printed. Two repairs, either
     sufficient: report on a clock that runs regardless (the per-N-seconds profile line
     is the house pattern), or gate on the arm's EFFECT with arms differing by one
     variable — part 34's atlas going 46.8750% -> 0.0038% zero between two otherwise
     identical runs IS the engagement proof, and a stronger one than any counter.

285. **A LIVE-PROCESS MEMORY DUMP HAS A TIME, exactly like a capture's memory records
     (280).** Dumping the texture a defect sampled MINUTES after the operator's F9 reads
     whatever the streaming heap holds NOW — recycled content that decodes as plausible
     garbage and supports any theory you brought to it. Part 35's first dumps were taken
     late and nearly convicted the wrong subsystem. The working protocol: the operator
     presses F9 and STANDS STILL, and the dump fires within seconds, keyed off the F9's
     own census. Same permission model as gdb, no ptrace stop, and the census supplies
     address + extent + format so the dump is self-describing.

286. **WHEN EVERY READER OF THE BYTES IS EXONERATED, STOP INSTRUMENTING READERS AND
     TRACE THE WRITER.** Part 35 killed five theories about who mis-READ a streamed
     texture — the shadow term (atlas healthy, patches stick to the surface), a VFS
     seek/read race (overlap counter: 0 across two sessions), a stale texture cache
     (content guard: 4 of 92.7M hits), a snapshot age fallback (cannot fire), and one
     misattributed draw — and the bytes were still garbage IN GUEST MEMORY at sample
     time. Every refutation was worth having, but the fifth should have prompted the
     pivot the first one already licensed: the defect is upstream of every reader, in
     whoever COMPOSED the bytes. The instrument changes accordingly: not another arm on
     the sampling path, but a write watch on the page (who stores to it, from where) or
     the engine's own narration of the system that builds the asset.

287. **A JUNK SCORE IS NOT A PICTURE — DECODE AND LOOK BEFORE BELIEVING THAT MEMORY
     HOLDS GARBAGE.** Part 35's scorer flagged the odd-extent DXT5 "impostor sheets"
     as black/white banded junk and the whole striped-material item was framed as "the
     CPU composes garbage into them; trace the writer". Part 36 decoded one (two-minute
     script, tiled layout + the alpha plane) and it is a coherent billboard alpha-cutout
     — WHITE colour endpoints with the content in ALPHA, which is exactly what a
     greyscale-with-extremes heuristic must flag. Worse, the bytes were BYTE-IDENTICAL
     (md5) to the bytes hardware sampled for the same material in the R3 trace, which
     exonerates the writer in one measurement — the strongest exoneration there is,
     available the whole time for the cost of one decode. The kickoff even carried the
     warning ("triage only; decode and LOOK before claiming") and the hunt was scoped
     off the score anyway. A scorer's verdict licenses a LOOK, never a hunt.

288. **CONTENT-HASH MATCHING AGAINST A HARDWARE FRAME EXONERATES ONLY THE MATCHES.**
     Matching every texture our blotch frame bound against every byte-carrying fetch in
     the same-site R3 trace: 226 of 459 byte-identical — 226 textures proven correct in
     one census. But the 233 unmatched are NOT 233 suspects: the two frames differ in
     camera, time and streaming state, so render targets, post surfaces and any asset
     the other frame didn't bind can never match. An unmatched entry is "unadjudicated",
     not "wrong" — the census narrows the field, it does not name the defect.

289. **A GUEST CALL MADE FROM A KERNEL-IMPORT HOOK RUNS ON A THREAD THE ENGINE NEVER
     PREPARED.** Part 36 called the title's own `setplayerpos` path from the pumps that
     live inside `XamInputGetState`, and it faulted identically every time: the chain
     reads the engine's per-thread context out of **TLS slot 8** (4,581 call sites read
     it, 4 write it, all in CRT thread-startup) and dereferences it unchecked, and no
     input-polling thread has it. Choosing a better MOMENT inside such a hook cannot
     help — it is the wrong thread, always. The fix generalises: **hook the accessor for
     the context itself and do the work there**, because any thread executing that
     accessor is by definition a thread that has it, which makes the qualification test
     and the call site the same object. Restore the hooked function's own return value
     around the detour, or every one of its callers gets your callee's r3.
     Corollary for diagnosis: `addr2line` on the RAW host pc (gotcha 57) named the
     faulting guest instruction in one step, and the three-instruction callee it sat
     after was the whole answer.

290. **A FIELD THE ENGINE REWRITES EVERY FRAME IS AN OUTPUT — WRITING IT CANNOT MOVE
     ANYTHING.** The player's four position fields were written both by the title's own
     setter and directly, and dumped immediately afterwards: all four still held the old
     value, and one of them differed from the others in the last digit, which is the
     tell — the engine was updating them continuously from upstream (a Havok body). The
     check that settles it costs one print: dump the fields right after the write, on
     the same thread. Do not read a DIFFERENT field to verify a write (the getter here
     reads +0x1C while the setter writes +0x620, so "unchanged" was consistent with both
     success and failure and proved neither).

291. **PAIRING DRAWS ACROSS PLATFORMS BY VERTEX COUNT IDENTIFIES A MESH, NOT AN
     OBJECT — content-ID the textures instead.** Part 37 needed "the tanker's draw" in
     two frames of the same scene from two renderers. Pairing by (verts, shaders) twice
     produced confident wrong answers: first the zone's street-clutter chunk (present
     in both frames because it is the same street), then an NPC's HEAD whose atlas was
     byte-identical on both platforms for the honest reason that the same NPC stood in
     both frames. What worked: decode the hardware frame's large textures, find the
     target BY LOOKING (a truck skin is unmistakable), then md5 the content into the
     other frame's dump to get the address, and let the census name the draw that binds
     it. A vertex count is a mesh identity; meshes are shared and scenes overlap. The
     part-36 warning (§6bk) said this about near/far pairs of ONE run; it is just as
     true across platforms, and it cost two wrong attributions before it was applied.

292. **A CORRECTION FOR YOUR OWN COPY PIPELINE CAN DOUBLE-CORRECT WHAT THE GUEST'S
     SHADER ALREADY COMPENSATES — model the whole chain state-by-state before adding
     one.** The striped-material class (item 0s, three parts of work) was the
     runtime's own 16-bit-texcoord unswizzle mask: the 8-in-32 dword reverse leaves a
     16_16 pair transposed, but that is exactly the state the real Xenos fetch hands
     the shader, and the Xenos COMPILER already emits a compensating .yx destination
     swizzle on ~87% of such fetches (known since the Fable 2 census, translated
     faithfully all along). The mask corrected the pair a second time, so lightmap UVs
     arrived (V,U) and baked prop shadows painted hard-edged black camo over the
     tanker, Dick's far LOD and the pawnshop boards. Two aggravations worth the
     ledger: the mask was JUSTIFIED originally by a metric later retracted as noise
     (§6h -> §6k), and the correct arm (§6n's mask-off run) had already been measured
     and dismissed as "no effect" because whole-frame statistics cannot see a defect
     localized to a few dozen lightmapped draws. When your emulation of a hardware
     stage matches the hardware's OUTPUT state, the guest's own compensations are
     complete and every runtime-side "fix" after that point is a defect. The
     four-line state table (guest bytes -> hw fetch -> shader swizzle) settles it on
     paper; run it before publishing any endianness/swizzle correction.

293. **A REPAIR GATED ON A CENSUS IS GATED ON THAT CENSUS'S ROUTE — a cache-staleness
     rate measured on a short fixed run says nothing about a play session.** The
     texture cache's revalidate repair existed since part 35 and stayed off because
     its own census read "4 stale of 92,730,622 hits" — measured on a 400 s headless
     run standing near one location, where streaming barely recycles an address. A
     real operator session recycles addresses all evening, and the stale share became
     "almost everything up close wears a random texture" (a tanker wearing a brick
     wall; guest memory at the address holding a THIRD asset by dump time). The
     gotcha-50 family says a rate is a fact about its afternoon; this is the sharper
     form: a rate is a fact about its WORKLOAD, and a repair whose defect scales with
     session length and area coverage must be censused on the workload that shows it
     — or shipped on correctness with the off-arm kept, which is what part 38 did.

294. **AN EXIT PATH THAT SKIPS THE STATS DUMP TURNS A WHOLE SESSION'S COUNTERS INTO
     NOTHING — audit every way the process ends.** The renderer's counter dump ran
     only on paths that returned through main; the window-close path called
     `std::_Exit` directly (correctly — guest threads are live) and a full operator
     evening's alpha-mode census vanished with it. The counters exist precisely for
     the runs a human drives, and a human ends a session by closing the window: the
     one exit path most likely to carry interesting counters was the one that dropped
     them. One call before `_Exit` fixes it; the check is to enumerate every
     process-exit site and ask which reports run on each.

295. **A DECODED FIELD THAT NOTHING READS IS INDISTINGUISHABLE FROM A FIELD THAT DOES
     NOT EXIST — and the way it presents is "the hardware must not use this."** The
     Xenos texture fetch constant names TWO addresses: dword1's base, which holds mip
     level 0 and nothing else, and dword5's separate MIP ADDRESS, which holds levels
     1..n. `DecodeTextureFetch` had parsed `mipMin`/`mipMax` since phase 5 and no line
     of the renderer read either, `CreateImage` hardcoded `mipLevels = 1`, and no
     census on either side printed a mip column — so for thirty-four parts the whole
     mip chain was invisible, and every discussion of distant surfaces proceeded as
     though the guest had supplied one level. It took one column added to two censuses
     to find that hardware declares a chain on the majority of its fetches, up to nine
     levels deep. **The check: for every field a decoder parses, grep for a READER.**
     A parsed-and-unread field is worse than an unparsed one, because its presence in
     the struct reads as support.

296. **AN INSTRUMENT THAT REPORTS 100% IS USUALLY REPORTING ON ITS EMITTER, NOT ON ITS
     SUBJECT.** "How many of our pixel shaders contain a discard?" returned **324 of
     324** — which looks like a decisive answer about materials and is a fact about
     XenosRecomp: every translated pixel shader carries the same
     `SPEC_CONSTANT_ALPHA_TEST` clip unconditionally, live only when the pipeline key
     sets the alpha-test bit. The comparable question asked of hardware's own microcode
     (R4's 208 dumped pixel shaders, grepped for `kill`) returns **1**. Same question,
     two orders of magnitude apart, because one side was counting scaffolding. The
     sibling of gotcha 25: a grep that cannot MISS is as uninformative as one that
     cannot match, and a saturated count should be read as a question about the
     generator before it is read as a finding.

297. **THE SIGNAL EVERY RECIPE USES TO END A RUN WAS THE ONE EXIT PATH WITH NO REPORT.**
     Gotcha 294 fixed the window-close path and left its headless twin open: every
     headless recipe in this project ends with `timeout`, which is SIGTERM, whose
     default action is to die silently — so the arm that carries the counters worth
     reading was the one arm that never printed them, and part 39 initially read a
     brand-new counter as "did not fire" when it had simply never been dumped. Install
     the handler for SIGTERM and SIGINT, not only for the graceful path. Generally:
     enumerate the ways the process can END, not the ways it can finish.

298. **"DISTINCT COLOURS" REWARDS ALIASING, so a fix that removes aliasing scores WORSE
     on it while being more correct.** Part 39 registered the prediction that uploading
     the guest's mip chain would RAISE the outdoor era-median distinct-colour count,
     because "distant surfaces gain detail" sounds like more colour variety. It fell —
     and falling is the right direction: sampling an unfiltered level 0 at high
     minification manufactures colour variety out of aliasing, and a properly filtered
     mip removes it. The statistic was measuring the defect as though it were signal.
     **The check that saved it was a reference we did not write**: hardware's own R4
     frames read meanLuma **58.6** and distinct colours **127,574** where ours read
     75.9 / 147,119 without mips and 74.6 / 143,803 with them, so the change moves BOTH
     statistics toward hardware. Before registering a prediction on an aggregate, ask
     which DIRECTION the correct answer lies in — and where possible register it against
     the oracle's value rather than against "more" or "less".

299. **A `CZ_VK_FRAME_STATS` FILE BEING APPENDED TO IS NOT A RUN, AND IT DOES NOT SAY
     SO.** Part 39 read one arm's stats file while its run was still writing, got 7,896
     era frames instead of 8,279, and computed a within-arm null of **2.81%** on mean
     luma. Complete, the same pair reads **0.06%** — a 47x difference in the noise floor,
     from a file that parsed cleanly and looked like a finished run. That inflated null
     then swallowed the effect and the A/B read "INSIDE the null" when it is in fact
     9x its floor. **Check the run has EXITED before reading its stats** (the process is
     gone / the counter dump is in the log — gotcha 297 is why that dump now exists on
     the `timeout` path), and treat an era frame count well below its sibling's as the
     tell.

300. **A NULL MEASURED FROM ONE PAIR IS A PROPERTY OF THAT PAIR, AND TWO ARMS NEED NOT
     HAVE THE SAME NULL.** `frame_era_medians.py` takes exactly two null runs and one
     arm, so at three runs an arm somebody PICKS which pair supplies the floor — and the
     pick can decide the answer. Compute the WITHIN-arm spread of EVERY arm and require
     the between-arm difference to clear the WORST of them; `tools/frame_arm_spread.py`
     does exactly that and refuses to choose. Sibling of gotcha 159 and of 50/51/86.

301. **A GUARD BUILT TO CONFIRM A RULE IS ONLY WORTH BUILDING IF IT CAN REFUTE IT — AND
     THE ONE THAT REFUTED IT SAVED A MEASUREMENT, NOT JUST A PICTURE.** Part 39's mip
     layout was verified by hand against two of hardware's chains and then shipped with
     a cheap runtime check: does each level's average match the level above it? The
     expectation was zero hits, turning "two textures" into a census. It read **254 of 1,818 chained
     textures** — and the EIGHT lines it prints are a capped sample, which the first
     write-up of the finding recorded as the count (gotcha 109 again, in the same hour it
     was quoted). All eight printed share one signature — level 1 at almost exactly
     **1/3** of its base's luma, on every texture whose level 1 is narrower than a macro
     tile. A consistent ratio is a
     wrong PITCH, not corrupt data. Both hand-verified chains happened to have a level 1
     exactly 32 blocks wide, so neither could have shown it.
     **The part that matters: the A/B had already been run on the binary that bound those
     levels, and re-running it with them rejected did not qualify the result — it ERASED
     it.** Mean luma went from **−1.35% at 2.1x its floor (resolved)** to **+0.36%
     (unresolved)**: the sign flipped and the magnitude collapsed, so the entire measured
     effect was the bug rather than the feature. Worse, the bug had an alibi — it moved
     the frame TOWARD hardware's darker reference, so the oracle agreed with it. **A
     result that matches the oracle for the wrong reason is the most dangerous shape a
     measurement takes**, and nothing in the A/B could have separated the two. Build the
     invariant check into the code the same day you build the feature, and re-run every
     number when it fires.

302. **IDENTIFYING A MATERIAL BY ITS FORMAT SIGNATURE INSTEAD OF ITS CONTENT IS GOTCHA 291
     ONE LEVEL UP, AND IT COSTS A WHOLE INVESTIGATION.** Part 39 needed the shard-tree
     material, selected the draws that bind a DXT5+DXN pair (176 of them, one shader, one
     512x512 DXT1 albedo), and built an entire case on it: byte-identical to hardware, an
     exactly matching blend split, matching vertex histograms, therefore "state and inputs
     agree and the fault is in the shading". Decoded at the end — because the operator said
     the trees were still broken — that albedo is **HAIR**. Every conclusion was about a
     character material. **DECODE AND LOOK before you build an argument on a texture**
     (gotcha 287 said this; 291 said pair by content, not by a proxy). A signature is a
     hypothesis about identity, and it needs the same confirmation as any other.

303. **COMPARING THE REGISTERS THE GUEST SET, ACROSS TWO PLATFORMS RUNNING THE SAME GAME,
     PROVES NOTHING — THEY AGREE BY CONSTRUCTION.** The same part-39 pass reported that
     hardware and this runtime "agree on the alpha test, alpha-to-mask and blend mode" at
     the disputed draws. Of course they do: both numbers come from one title's own command
     stream. The comparison that means something is between what the guest ASKED and what
     each renderer DID — our pipeline state, our sampler, our translated shader. The
     operator caught this immediately ("it's impossible that the GPU sees them exactly the
     same") and was right. When a cross-platform diff of guest-set state comes back
     identical, that is the null result you should have predicted, not evidence.

304. **A FRAME COUNTER INCREMENTED BY THE SWAP MAKES "ARM THE NEXT FRAME" NAME A NUMBER
     THE DRAW PATH NEVER SEES.** The draw-ID pass was armed from the present path as
     `drawIdFrame = R->frame + 1`, exactly as the existing capture and census arms are.
     `++R->frame` happens at the SWAP, so the draws of a frame are recorded while the
     counter still holds the previous frame's value: the flag was compared against a
     number that had already been skipped past by the time any draw looked. The pass
     never ran, for three consecutive test runs, **while its output was being read as if
     it were a map** — the surface it "produced" turned out to be an ordinary buffer whose
     small values happened to look like plausible draw indices. What settled it was
     dumping the same address from a run with the instrument OFF and finding the two
     images IDENTICAL. Arm with a FLAG consumed by the first draw that sees it; a flag
     cannot be off by one. And put a counter on the arm (gotcha 151) — "0 draws painted
     an index" would have said this in the first run instead of the fourth.

305. **A DIAGNOSTIC THAT REPLACES WHAT A DRAW WRITES MUST NOT CHANGE WHICH DRAWS WRITE.**
     The first draw-ID pass forced the colour write mask open so the index could not be
     partially written. That let this title's depth-only prepass draws (`mask=0`, 38.6% of
     all draws) paint indices over 31.5% of the map, and the three "biggest visible draws"
     it reported were all draws that write no colour at all. An ID map is a map of what was
     PAINTED: substitute the fragment shader, disable blending, and leave every other piece
     of state — mask, depth test, depth write, cull — exactly as the draw had it.

306. **A GUEST ADDRESS IS A FACT ABOUT ONE BOOT; A SHADER HASH IS A FACT ABOUT THE
     MATERIAL.** Part 39 identified the foliage material from an operator capture and
     wrote down its six texture addresses. Part 40 replayed the same route headlessly
     with `CZ_VK_TEX_DUMP_ADDR` pointed at them and decoded the result: **barbed wire**.
     The streaming heap hands the same address to a different asset in a different boot,
     so every address a capture yields has a shelf life measured in one process
     lifetime. A pixel- or vertex-shader hash is a hash of the microcode and is
     identical in every boot, which makes it the only handle that survives the trip from
     "the operator saw this" to "reproduce it headlessly". Build the instrument that
     keys on it (`CZ_VK_TEX_DUMP_PS`, `CZ_VK_ONLY_VS`), and note that the earlier
     finding that streaming addresses were *stable* across boots (part 36, 703 of 712)
     was measured between two runs of the same route on the same day — it is not a
     licence to carry an address across a route change.

307. **AN OVER-ACCEPTING SCANNER PRODUCES A HISTOGRAM, NOT A CENSUS, AND THE HISTOGRAM
     WILL LOOK LIKE A FINDING.** Asking whether Case Zero's microcode declares a
     non-zero vertex-fetch `exp_adjust`, part 40 first scanned every dword triple for a
     vfetch-shaped word pair — opcode 0, the architecturally-required bit 19, a known
     format — and got **234 non-zero of 607**, with a plausible-looking spread of values.
     It was noise: the scan accepts ALU words by construction, and the docstring said so
     while the number was being read as evidence. Re-run through
     `tools/synth_shader_container.py`'s own control-flow walk — the parser the shader
     cache is actually built with, so every instruction it reads is one the translator
     reads — the answer is **345 vertex fetches across 99 vertex shaders, exp_adjust
     zero on every one**. If a parser for the thing already exists in the project,
     import it; a structural approximation of a parse is only admissible when its
     over-acceptance is bounded, and "it found a self-test plant" bounds nothing.

308. **A REGISTER INDEX IS A DECODE, AND A REFUTATION MEASURED ON THE WRONG REGISTER
     REFUTES NOTHING.** Case Zero's `xenos.h` placed RB_COLORCONTROL at 0x2205 by
     assuming the four per-RT blend controls sit contiguously at 0x2201..0x2204; the
     real map interleaves them (0x2201/0x2205/0x2209/0x220D) with COLORCONTROL at
     0x2202. Everything downstream inherited the error in both directions at once:
     the runtime's alpha test (part 38) read 0x2205 and never fired on anything, and
     the analysis tool read 0x2205 across 40,703 hardware draws and concluded
     "hardware never enables the alpha test" — a refutation that closed item 0t's
     correct explanation for a whole part. Three defenses, all cheap: (a) **cross-check
     any hand-built register map against a sibling port's** before building on it —
     Fable 2 had 0x2202, and one grep would have surfaced the disagreement; (b) an
     emulation that "changed nothing" has a COUNTER, and a counter that reads zero in
     every log is telling you the feature never engaged (gotcha 151's shape, worn by a
     shipped feature rather than an arm); (c) a register can be identified
     EMPIRICALLY — RB_COLORCONTROL's value carries the 0xAA alpha-to-mask offset
     signature in its top byte on every draw, and histogramming both candidate indices
     over one hardware trace settled it in a minute. The tell that finally broke it:
     hardware writing a meaningful RB_ALPHA_REF (0.502) on draws whose test read
     "disabled" — state nobody sets for a feature nobody uses.

309. **A GLOBAL RESOURCE SERVES EVERY SEMANTIC AT ONCE — set a per-thing property
     globally and you set it on the thing that cannot tolerate it.** Part 41 enabled
     16:1 anisotropic filtering on Case Zero's one global sampler, reasoning "every
     fetch publishes sampler index 0, so index 0 is where aniso goes". The very first
     capture showed dark speckle across the whole frame: index 0 also serves the
     SHADOW ATLAS lookups, and anisotropically averaging depth values before a manual
     depth comparison makes the comparison flicker per pixel. Hardware was never in
     danger of this because the property is PER FETCH CONSTANT — and the census that
     settled it (histogram dword3 bits 25..27 over 621 distinct fetches) showed the
     title asking for 4:1/8:1 on albedo and aniso=0 with POINT filters on the shadow
     atlas. The fix was never "tune the global value"; it was to stop the value being
     global. Transferable form: before promoting any per-resource field to a global
     default, enumerate the consumers of the global and ask which one the new value
     breaks — a depth-comparison path is the usual victim.

310. **A METRIC CAN FLAG A DEFECT WHILE MISLABELING IT — read a surprising drop as
     "something broke", not "the change did nothing".** The global-aniso arm's
     registered prediction was "sharpness rises"; the speckled arm read -19%
     (4.351 vs 5.313/5.389, null 1.4%). The metric was not blind — it moved 13x its
     noise floor — but its LABEL was wrong-way-round: without the matched-F9 eyeball,
     a large drop on a "should rise" prediction reads as "aniso is ineffective or
     mildly harmful, drop the item", when the truth was "aniso broke an unrelated
     subsystem". A refuted prediction is a fork with three prongs (effect absent /
     effect reversed / a DIFFERENT mechanism moved the number), and only a picture or
     a mechanism-level instrument can pick the prong. Pair every aggregate metric
     verdict with one matched-viewpoint LOOK before acting on it.

311. **BEFORE TRUSTING NUMBERS READ OUT OF A TRACE'S MEMORY RECORD, RENDER THE
     RECORD AS A PICTURE.** Part 41 read "hardware's scene depth" out of an R4
     memory record, measured values 0.0-0.35, and built three encoding theories on
     them — the record, rendered as an image, was the PREVIOUS FRAME'S COMPOSITED
     SCENE in greyscale with the HUD text legible ("20 KILLED"). Gotcha 280's rule
     (a trace cannot carry a surface produced inside the traced frame) had already
     been learned on the shadow atlas; this is its second disguise: the pre-frame
     record at the address existed and decoded without error, so nothing FAILED —
     the bytes were simply a different surface (the address is colour-aliased,
     gotcha 203). The two-minute picture test (gotcha 287) is what caught it, and
     it caught it AFTER an hour of arithmetic that a picture would have prevented.
     Corollary for the provenance guard built in part 32: "no resolve to this
     address in the trace" does NOT certify a record as a sound oracle when the
     title resolves there every frame — it certifies the OPPOSITE: the in-frame
     product is missing and the record is whatever lived there before.

312. **A STATIONARY HOLD IS THE EXPERIMENT THAT SPLITS A RATE DEFECT FROM A
     THRESHOLD DEFECT — run it before building any "make it faster" fix.** Part
     42, item 00i: the flat-texture class had a named rate candidate on the board
     for 14 parts (the `KeSetBasePriorityThread` no-op starving a decompression
     thread) and a plausible sibling (file-IO latency). One fixed-camera,
     fixed-position run — 20 F9s at 16 s intervals — showed the same three
     thumbnail-quality textures unpromoted across 13 censuses and 2.3 minutes,
     while the walk evidence shows them promoting instantly on approach. A
     rate-limited system fills in when demand stops moving; a threshold or
     budget decision holds forever. Every "it loads late" symptom has both
     readings, the fixes are disjoint, and the discriminating run costs ten
     minutes with no code written.

313. **WHEN TWO DEFECTS SHARE PIXELS, DECIDE WHICH ONE OWNS THE DAMAGE BEFORE
     READING ANY CROSS-PLATFORM COMPARISON.** Part 41 compared our far field
     against hardware's and recorded a blocking contradiction: same DoF shader,
     same constants, yet hardware's 40-60 m storefront "legible" where ours is
     mush. Part 42 dissolved most of it: the comparison had read TEXTURE damage
     (item 00i's patternless walls — nothing survives a 95% half-res lerp) as
     BLUR damage (item 0u), because both classes land on the same distant
     pixels. High-contrast signage survives the same lerp on hardware, which is
     all "legible" was. The A/B admissibility rule (two arms must render the
     same thing) has a cross-ITEM form: a platform comparison for defect A is
     admissible only where defect B is absent or equal on both sides — and the
     part-28 LOD verdict ("hardware's transitions are invisible because its
     textures are not broken") was the same lesson pointing the other way.

314. **AN INSTRUMENT THAT PRINTS GUEST BYTES CAN POISON ITS OWN LOG'S
     SEARCHABILITY — grep the file binary-safe before believing the
     instrument absent.** Part 43's zone probe printed a directory OBJECT as
     %s; the NULs it emitted made grep treat the whole log as a binary file,
     and two runs were read as "the hook never fired" — an hour went into
     verifying link-time symbol override that was never broken, while 263
     probe lines sat in the file. `grep -a` / `tr -d '\0'` first. This is
     gotcha 25 (a grep that cannot match is not a clean result) in the form
     where YOUR OWN instrument is what broke the match — and the double
     lesson: never print guest memory as %s without sanitizing it, because
     the failure lands not in the line that printed it but in every later
     search over the file.

315. **A STATE COMPARISON NEEDS MATCHED HISTORIES, NOT JUST MATCHED
     POSITIONS.** Item 00i's "hardware is fully textured where we are flat"
     was measured between a FRESH DebugJump (ours) and a warm session that
     had WALKED to the capture point (hardware's R4 sweep) — and the decision
     under test runs once per zone load and snapshots the camera, so the two
     arms' histories, not their positions, determine the outcome. Part 42's
     four-way corner survived every check on our side (part 43 verified every
     input live) precisely because the divergence was never in the
     computation — it was in the state the two sessions brought to it. Before
     reading any A-vs-B where either side carries accumulated state
     (streaming, caches, promotion), write down how each arm GOT there.

316. **A PARTIAL WRITE IS NOT A WRITE — dataflow over an ISA with write masks
     must track COMPONENTS, not registers.** Case Zero's synth container tool
     derived "which PS registers are interpolator inputs" as read-before-write
     over a flat set of register numbers, so a `tfetch2D r0.__xy` (destination
     swizzle KEEPS .xy, writes only .zw) removed r0 from the inputs sixteen
     instructions before the shader sampled its diffuse at r0.xy. The
     translated HLSL zero-initialised the register, the surface sampled ONE
     TEXEL forever, and the class — 217 of 333 pixel shaders — presented as
     flat-colour props and facades that survived SEVEN input-side refutations
     across three parts, because every input really was correct. Two
     corollaries paid for separately: the idle scalar co-issue slot is
     RetainPrev (50), not Adds (0), so "opcode nonzero" is the wrong presence
     test and would drop a real `adds` feeding a `*_prev`; and the old
     analysis never tracked scalar co-issue DESTINATION writes at all, so its
     input lists carried spurious registers whose only writer was the scalar
     pipe. Transcribe operand component semantics from the translator's own
     printer rather than re-deriving them — the two ends cannot then disagree.

317. **AN UNEMULATED FEATURE'S EXCUSE HAS A DOMAIN — check the domain, not the
     feature.** Case Zero's renderer declined to emulate ALPHA_TO_MASK with a
     written reason: "the draws hardware sets it on also set the alpha test, so
     the clip covers them". True in general, and false for every draw that
     matters, because this title's foliage sets **`RB_ALPHA_REF = 0.0`** — at
     which point the alpha test keeps everything and A2M is doing all of the
     work alone. The material is DXT4/5 (fractional alpha), so what should have
     been fractional coverage was written at full opacity, and a canopy of soft
     feathered leaf cards came out as hard-edged near-black shards. When a
     declined feature is justified by "something else covers it", write down the
     range of inputs over which that is true and CENSUS the title against it;
     the excuse and the counter-example lived four parts apart in the same file.

318. **A SUBSET-OF-ONE-DRAW SYMPTOM ELIMINATES EVERY PER-DRAW INPUT AT ONCE,
     AND THAT IS THE CHEAPEST CUT AVAILABLE — take it first.** Selecting the
     defective pixels and the correct pixels separately out of one frame and
     reading BOTH through the draw-ID map answered, in one run, that they came
     from the same draw — which retires the constants, the bound textures, the
     render state and the pipeline together, without measuring any of them.
     Three parts of this port were spent measuring per-draw inputs one at a time
     on defects that this cut would have re-scoped in an afternoon. Do the
     both-populations attribution before the first input comparison, not after.

319. **PREFER A REPRO THAT ALREADY HAS AN ORACLE IN THE REPOSITORY.** The tree
     defect was believed to need an operator standing at a gas station. It also
     reproduces on the TITLE SCREEN — 120 s, no input, near-static camera — and
     that screen is one of the round-1 hardware screenshots (E3), so the arm,
     the control and the ground truth are all self-servable. A static repro also
     buys the thing a moving one cannot have: two runs land on the same camera,
     so a draw-ID map from one run can be read against a picture from another.
     Before accepting "this needs a play session", check the menus, the
     attract loop and the title backdrop — they are usually the same renderer on
     the same assets, and the screenshot set was captured there first.

320. **A PROFILER'S PHASE SHARE MOVES WHEN THE OTHER PHASES DO — quote
     MILLISECONDS.** Part 47 took 13 ms out of the texture phase, and the same
     profile window then reported `record` +34%, `constants` +43%, `other` +38%
     and `outside` +68%. None of them had moved: a share is a share OF THE
     FRAME, so shrinking one phase inflates every other one's percentage while
     its cost is unchanged. The first read of that A/B looked like one item
     fixed and four regressions. Multiply the share by the window's frame time
     before comparing arms, and only use the share to decide what to look at.
     Sibling of 238 (a mean frame time measures the pacing floor) and of 228
     (nested phases counted twice): a profiler's own arithmetic is a thing to
     re-derive, not to read.

321. **TWO ARMS OF ONE A/B DO NOT WALK THE ROUTE AT THE SAME SPEED, so pooling
     their profile windows measures the ROUTE and calls it noise.** The faster
     arm gets further in the same wall clock and spends its windows in denser
     places. Pooling every window above 1,500 draws produced a within-arm
     "noise floor" of 58%, which was not noise at all — it was a safehouse
     window averaged with a crowd window. Restricted to a matched 3,000-8,000
     draws/frame band, the same runs' baseline spread is 1-2% and the effect is
     unambiguous. Any per-window statistic compared across arms needs a matched
     band on the variable that drives it, exactly as the era-median tooling
     already does for pictures (gotcha 254's family, applied to timing).

322. **A GATE THAT PASSES IDENTICALLY BEFORE AND AFTER YOUR CHANGE HAS NOT
     TESTED YOUR CHANGE — and the code you replaced is still compiled in, so use
     it as the oracle.** Part 47 rewrote the command processor's register-write
     loop. Both PM4 boundary oracles passed, and would have passed just as well
     had the rewrite been wrong: they verify packet-LENGTH and indirect-walk
     arithmetic, which the rewrite does not touch. A picture correlation could
     not see it either, because a wrong register produces a plausible wrong
     picture. What settled it was running the OLD per-dword path alongside the
     new bulk one and comparing every dword — 0 mismatches over 152 M — with a
     poison arm first, to show the check could fail at all (30). Before trusting
     a gate on a change, ask what it would report if the change were wrong; an
     incumbent implementation is an oracle you already own, and it is the one
     kind of second opinion that is not two of your own new components agreeing.

323. **A PER-FRAME REDUNDANCY FACTOR ESTIMATED FROM PER-RUN TOTALS IS WRONG BY
     THE LENGTH OF THE ROUTE.** "How many times does one frame re-check the same
     texture?" was estimated at 2x by dividing a frame's 6,790 guard checks by
     the 3,266 distinct addresses the RUN had touched. Measured per frame it is
     **15.1x**, because a run visits many places and a frame visits one. That
     factor was the entire size of the item, so the estimate would have priced a
     13 ms fix at under 1 ms and probably killed it. When a ratio's numerator is
     per-frame, its denominator must be too — and if the per-frame denominator
     is not instrumented, that is the measurement to add first. Same family as
     242, one level down: there the statistic was fitted to the reachable
     population, here to the reachable duration.

324. **A HOT LOOP WITH A SERIAL DEPENDENCY IS LATENCY-BOUND, AND THE FIX IS MORE
     ACCUMULATORS, NOT MORE BYTES PER STEP.** The content guard's fold already
     read 8 bytes an iteration and still ran at 9 GB/s, because
     `h = (h ^ v) * PRIME` puts a ~5-cycle multiply on the critical path per
     step — about 1.6 bytes/cycle whatever the machine's load bandwidth is.
     Four independent accumulators gave the out-of-order engine four chains to
     overlap: **9.01 → 35.68 GB/s, 4.0x, same bytes read.** It looked like a
     bandwidth problem and was an ILP problem, and the tell is that widening the
     step had already been done and had not helped. Before optimising a byte
     loop, ask whether each step depends on the previous one's result.
     **And measure the SENSITIVITY as well as the speed**: a hash that got faster
     by noticing less would be a silent disaster with no symptom until a stale
     buffer reached the screen. Flipping one bit at 676 positions across a 64 KB
     buffer and confirming **0 misses on both the old and the new fold** is what
     makes the speed claim safe to ship.

325. **A COUNTER THAT NOTHING READS IS NOT AN INSTRUMENT, AND ITS SUBJECT IS AS
     UNMEASURED AS IF IT HAD NEVER BEEN COUNTED.** `Pm4_OpcodeCount` and
     `Pm4_TypeCount` have been incremented on every packet since phase 4 and are
     called from nowhere in the runtime. So "which packets cost the 16.6 ms of
     the operator's PM4 walk" was unanswerable while the answer sat in memory,
     and part 48 has to start by printing numbers this project has been
     collecting for thirty parts. Distinct from gotcha 25 (a grep that cannot
     match): there the emitter was missing, here the emitter is fine and the
     READER was never written. When adding a counter, add its print in the same
     commit — an unprinted counter is a cost with no benefit, and it reads as
     coverage that does not exist.

326. **A PROFILER PHASE NAMES A SCOPE, NOT A SUBSYSTEM — and gotcha 238 said so
     nine parts before anyone acted on it.** `g_prof.streams` wraps only the
     stream COPY, so the cross-frame store's content guard — **81.65 MB of
     hashing in one frame of the operator's session** — was charged to `record`,
     the phase that encloses it. `streams` read 0.0% throughout and was cited for
     five parts as "the stream cache is closed, do not revisit". Both statements
     were true and together they were badly misleading. Gotcha 238 already
     contains the general form AND this exact example ("`streams` read 0.0% while
     the guard that replaced it doubled `record`"); it took until part 47's split
     of `record` for anyone to follow it. **When a phase reads zero, find the
     scope's boundaries before believing the subsystem is free** — and read the
     ledger for the case you are about to rediscover.

327. **THE THIRD ITEM IN TWO PARTS WAS FOUND BY SPLITTING A PROFILER PHASE, AND
     NONE OF THEM BY READING THE CODE.** Part 47 split `record` and found the
     stream guard; part 48 split `other` and found a `getenv` on the per-draw
     path; part 48's opcode census — a split of the PM4 walk by opcode — found
     that 28.7% of the packets are ring filler that does no work. In every case
     the code had been read before, by someone looking for exactly that kind of
     defect, and in every case reading did not find it. **Splitting a phase is
     cheap, it is reversible, and it is the highest-yield action available on an
     uninstrumented number** — so when a phase is above ~10% of a frame and has
     no breakdown, split it before forming any hypothesis about it. The corollary
     is the ranking rule: a phase with no breakdown should outrank a phase with
     one at the same size, because the second has already been looked at.

328. **A PLAN'S PREDICTION ABOUT WHAT IS INSIDE A PHASE IS A GUESS, AND IT WILL
     NAME THE THING THAT HAS A NAME.** `docs/perf-plan-part48.md` §5 predicted the
     pipeline-key `std::map` probe would be "most of" `other`, reasoning from a
     real precedent (part 47's sampler map). Measured: the probe is **16%** of
     `other`, the RESIDUAL is 45%, and the fetch walk is second. The error is
     systematic rather than unlucky — a plan can only name suspects that have
     names, and a residual by definition has none, so every such prediction is
     biased toward the named component and against the one that is actually
     costing. **Write the split before writing the ranking, not after**, and
     treat a plan's within-phase ordering as a list of candidates rather than as
     a priority order.

329. **`getenv` IS A LINEAR SCAN, AND ONE ON A PER-DRAW PATH IS INVISIBLE IN
     REVIEW BECAUSE IT LOOKS LIKE A CONSTANT.** `EnvOn("CZ_VK_NO_ALPHA_TEST")`
     read like a compile-time switch and was a walk of the environment block per
     draw; every other environment read in the same twenty lines was already a
     function-local static, which is what made the one exception unreadable. The
     grep that finds these is not `getenv` — it is **`getenv`/`EnvOn` NOT preceded
     by `static`**, and it is worth running once per performance part. The same
     line's other half is the matching idiom for counters: a name built with
     `snprintf` and then looked up in a `std::map<std::string, uint64_t>` is a
     per-draw allocation and a tree walk, and where the name has a small fixed set
     of values it should be that many literals with cached slots.

330. **A CHANGE CAN DO EXACTLY WHAT IT WAS DESIGNED TO DO, PROVE IT ON ITS OWN
     COUNTER, AND STILL BE A NET LOSS — because its counter measures the half it
     improved.** Part 48's item 2b gave the per-frame stream cache a generation
     stamp so an older entry became a miss overwritten in place instead of a
     `clear()` and a fresh allocation. Its counter was unambiguous: **97.7% of
     fills reused a node and allocations fell from 1,161,050 to 25,668 in a 20 s
     window, 45x.** The phase it lives in got **8.5% SLOWER** (16.7% on the index
     path), consistently across three runs an arm. The map stopped being emptied,
     so it grew from ~1,900 entries to ~7,000, and every one of the
     22,000-33,000 LOOKUPS a frame then walked a bigger table. The change
     optimised 1,800 inserts at the expense of 22,000 lookups. **Before building
     a cache change, count BOTH populations and price the one you are not
     touching** — and note the upper bound was arithmetic available beforehand:
     58,000 allocations a second at even 100 ns each is 0.6% of wall time, an
     order of magnitude below what the plan predicted the item was worth. A
     counter that only watches the operation you made cheaper cannot report the
     one you made dearer.

331. **A "MATCHED DRAW BAND" CAN STILL BE WIDE ENOUGH TO BE THE WHOLE EFFECT, AND
     THE FIX IS A NULL-CONTROL ARM RATHER THAN A NARROWER BAND ALONE.** Gotcha 321
     established the 3,000-8,000 draw band. Part 48 measured `record` in ns per
     draw and found it varies **1,204 → 1,033 across that band** — because
     per-frame costs amortise over more draws — while the four arms of one
     campaign populated the band at medians from 4,669 to 5,544 draws. So an arm
     that merely wandered into denser scenery read 8% "faster" on a per-draw
     statistic. What caught it was not suspicion but a **NULL CONTROL INSIDE THE
     CAMPAIGN**: `CZ_PM4_ATOMIC_COUNTERS=1` changes only the PM4 walk and cannot
     move `record`, and it was reading -5%. Narrowing to 4,000-6,000 took the null
     to 0.5-2.3% — which then IS the noise floor, measured rather than assumed, and
     the surviving 16.7% effect is eight times it. **Put an arm in every campaign
     that must read zero on the statistic you care about**, and narrow the band
     until it does.

332. **NOT ASKING FOR A THING IS NOT THE SAME AS ASKING FOR IT NOT TO HAPPEN — and a
     second clock only reveals itself when it becomes the SLOWER one.** This runtime
     created its SDL renderer without `SDL_RENDERER_PRESENTVSYNC`, with a comment
     explaining that the guest's swap rate is the frame clock and a vsync-paced
     present would be a second clock. It never told SDL *not* to vsync, and under a
     compositor — Wayland here — presentation is throttled to the display refresh
     whatever SDL was asked for. **It hid for forty-eight parts because the guest was
     capped at 30 fps by its own present interval the whole time**, so the display's
     clock was never the binding one. The afternoon that cap was lifted, it bound
     immediately, and its failure mode is the sharp one: with no triple buffering a
     frame taking just OVER the refresh period cannot present until the NEXT one, so
     the rate snaps 60 -> 30 with nothing between. **The operator diagnosed it from the
     shape of the number** ("pretty sure it's vsync") while the headless arm had been
     reading 62.5 fps throughout — no window, no compositor, and nobody had asked why
     headless and windowed disagreed. Set the negative explicitly, use the call that
     can FAIL (`SDL_RenderSetVSync`, 2.0.18+) rather than the hint that cannot, and
     **print what you actually got** (`SDL_GetRendererInfo`'s `PRESENTVSYNC`) rather
     than what you requested. The general form: whenever a headless and a windowed run
     of the same binary disagree on a rate, the window is in the path and that IS the
     finding.

333. **A CAP THAT IS NEVER REACHED IS INVISIBLE, SO RAISING ONE CAP EXPOSES EVERY
     OTHER CEILING AT ONCE.** Lifting this title's 30 fps present interval did not just
     raise the frame rate; it made three previously-unobservable limits observable in
     one afternoon — the compositor's vsync (332), our own 16 ms vblank period (which
     puts the ceiling at 62.5 fps by construction), and the present path's readback
     cost, which went 2.7% -> 4.7% of the frame simply because it now runs twice as
     often per second. **Expect a cap change to produce a QUEUE of newly-visible
     limits rather than one number moving**, and price the ones you did not intend to
     change before quoting the one you did.

334. **A SHARE TELLS YOU HOW MUCH OF A POPULATION SOMETHING IS AND NOTHING ABOUT ITS
     SHAPE — and for "coalesce the cheap ones" the shape IS the value.** Part 48's
     opcode census said **28.7% of every PM4 packet this title walks is type-2 ring
     filler**, a one-dword no-op, and part 50's plan priced "skip runs of it" at
     1.5-2 ms on the strength of that number alone. But 28.7% is equally consistent
     with one enormous run of padding and with 23,000 isolated dwords wedged between
     real packets, and coalescing is worth everything in the first case and precisely
     nothing in the second. **Nobody had measured the run length, so the histogram was
     built before the fast path**, and it repriced the item on the spot: mean run
     **2.3**, bimodally 28% singletons and 72% runs of 2-3, with a thin tail of ~1,100
     runs of 32-63 and *nothing in between*. Coalescing alone would have removed 57%
     of the calls, not ~100% — which is what moved the fix from the callee to the
     CALLER, where the header is already in hand and the test is free. The histogram
     also answered a question nobody had asked: **0% of it is at ring level**, so it
     is not the driver padding the ring to a wrap boundary, which is what "filler"
     suggests, but the title's own indirect buffers padded packet by packet. Two
     different producers, and only one of them was where the code expected it.

335. **A PROFILER'S OWN CLOCK READS LAND IN THE RESIDUAL OF THE SCOPE AROUND THEM, SO
     THE OUTERMOST SCOPE'S "UNNAMED" TIME IS THE INSTRUMENT.** This project's
     `ProfScope` reads the clock twice — once in the constructor, once at close — and
     subtracts each child's measured total from its parent so the columns are
     exclusive. Work through where the two reads fall: the constructor's read happens
     between the parent's `t0` and the child's, so it is inside the parent's interval
     and is NOT in `childNs`; the close read is inside the child's own total, so it
     lands in the child's named phase. **One whole clock read per nested scope
     therefore accumulates in the parent's residual and nowhere else** — and DoDraw's
     outermost scope is `other`, whose 206 ns/draw residual two separate splits had
     failed to name. It could not have been named by splitting, because it is not in
     any of the code being split. The general form is nastier than gotcha 7's: an
     expensive probe usually distorts the thing it reports, but this one **files its
     own cost under a name that invites you to go looking for a defect there**. Check
     it the cheap way — calibrate the clock call, count the scopes, multiply, and put
     the product next to the residual — and give the model a positive control that can
     refute it. **Here it did not**: `CZ_VK_PROFILE_EXTRA_SCOPES=8` added eight
     do-nothing scopes per draw and moved the residual **205 -> 397 ns, a slope of
     24.0 ns per scope against the 21.6 ns calibrated read**, and DoDraw's ~8 DIRECT
     children at that slope account for 192 ns of a 205 ns residual. Note which count
     goes into that arithmetic: only the scopes nested *directly* inside the one whose
     residual you are reading, not every scope the frame opens — a grandchild's reads
     land in its own parent, not in yours. And note the consequence beyond the one
     item: **every ms and ns/draw a profiled run reports is inflated by the profiler**,
     including the frame time, so a budget built from `CZ_VK_PROFILE` describes a
     frame slower than the one the player is actually getting.

336. **THE HYPOTHESIS THAT SAYS "THIS WORK IS WASTED" DESERVES THE SAME COUNTER AS
     THE ONE THAT SAYS IT IS NOT.** The stream guard hashes 26 MB a frame over 400-480
     "proven dynamic" streams to decide whether to copy them. The reasoning wrote
     itself: a stream proven to change is a stream we are about to copy anyway, so the
     hash is a whole extra read of the buffer to learn what the copy would tell us
     free — and always-copying would be both **cheaper and safer**, since a stream
     always copied can never be served stale. That is a strong argument, it names a
     real mechanism, and it is wrong. One counter over the proven set's own
     observations said **11-13% of them found the stream actually changed**: the guard
     saves the copy on ~88% of them and is doing exactly what it was built to do.
     The trap is that "this work is wasted" feels like a conclusion rather than a
     hypothesis, because it is an argument for doing LESS and therefore sounds
     conservative. It is not — deleting work that turns out to be load-bearing is the
     same error as adding work that turns out to be useless, and it is harder to
     detect afterwards because the cost shows up as bandwidth somewhere else. Price
     the population before removing it, and put the counter on the ratio your argument
     turns on rather than on the total it makes vivid.

337. **A MEASUREMENT CORRECTION IS NOT A SPEEDUP, AND THE TWO ARE EASY TO BANK
     TOGETHER BECAUSE THEY MOVE THE SAME NUMBER IN THE SAME DIRECTION.** Part 50's
     largest result by far was that `CZ_VK_PROFILE` costs **2-4 ms a frame** — so the
     frame this project had been quoting at 28.3 ms was really ~25-26 ms. Its only
     shipped optimisation was worth **~0.4 ms**, below the route's own noise floor.
     Both make the reported frame time smaller; only one of them is something the
     player experiences, and **the player never paid the 2-4 ms at all**, because
     nobody plays with the profiler on. Writing "part 50: −3 ms" would be false in the
     way that matters, and it would compound: the next part inherits it as a baseline
     and "wins" already claimed cannot be won again. State the delivered saving and
     the corrected baseline as two separate lines, put the corrected baseline in the
     plan's own table rather than in prose, and say in the same breath what the player
     would feel — here, nothing. The general rule: **when a number moves, say whether
     the WORLD changed or your VIEW of it did**, and never let a retraction of your own
     earlier measurement be scored as progress.

338. **A WALL-CLOCK FRAME TIME CANNOT TELL YOU HOW MANY CORES YOU ARE USING, AND A
     PROFILER THAT ONLY INSTRUMENTS ONE THREAD WILL NEVER RAISE THE QUESTION.** Thirty
     parts of performance work on this port produced frame times, phase shares and
     per-draw costs, all measured on the renderer's own thread, and not one of them
     could answer "is this a single-core problem?". The answer, when finally asked in
     one 25-second sample of `/proc/PID/task/*/stat`, was **2.46 cores of 16 — 15.4% of
     the machine — with 37 threads alive and twenty of them below 0.5%**, two threads
     carrying 70% of the CPU. That reframes every item in the live plan, all of which
     make ONE thread's work smaller. The trap is structural rather than careless: a
     per-phase profiler is written from inside one thread, so its columns always sum to
     that thread's time and always look like a complete account of the frame. It cannot
     represent a core that is doing nothing, so it never prompts you to look. **Ask for
     the process-wide core count before building a budget, not after** — it is one
     script and it decides whether the strategy is "make this smaller" or "move some of
     it somewhere else". Two corollaries that both bit here: a thread's CPU% does not
     distinguish WORKING from SPINNING, so a saturated guest thread is a lead and not a
     conclusion; and time in a residual phase like `outside` is not necessarily work —
     our pump was 79% busy, so a fifth of what that column reported was the thread
     BLOCKED, waiting on a producer that lives on a different core.

339. **A GROWING FILE READ MID-RUN IS A COMPLETE FILE THAT ENDS EARLY, AND IT LOOKS
     EXACTLY LIKE A HANG.** Half an hour into a six-run campaign, an arm's stats file
     ended at 96.7 s of a 330 s run while the baseline's ended at 328.8 s, with a third
     of the frames and two profile windows against ten. Read as a finished run that is
     the signature of a stall — and the arm in question was a control arm, so "the
     control hangs" would have been a serious finding about the change under test. It
     was a file being written at that moment. **Nothing in the file says which it is**:
     a truncated tail and a completed short run are byte-identical. Before reading any
     artifact of a long run, check that the run has ENDED — a completion marker in the
     driver's own output, not the file's mtime and not a guess from the clock — and if
     it has not, do not read the artifact at all rather than reading it carefully.

340. **A SYMBOL PROFILER OUTRANKS THE PHASE PROFILER YOU WROTE, AND IT CAN SEE THE
     THREADS YOURS CANNOT.** This project spent thirty parts refining a per-phase timer
     inside its graphics pump — splitting `record`, splitting `other`, splitting the
     split — and every one of those columns is, by construction, a scope somebody
     already suspected. `perf record -F 999 -p <pid>` for 30 s named the top cost in
     every thread of the process at once, in function symbols, and it disagreed with the
     phase profiler about our own pump: the phase table charges ~15 ms of a 22 ms frame
     to the draw path, while at symbol level `DoDraw` is 9.84% of the thread and a
     CONTENT GUARD is 16.79%. In a recompiled port it is better than that, because
     every guest function is a real symbol: the same profile read the TITLE's code and
     showed 84% of the busiest thread in one spin-wait. **Run the symbol profiler before
     writing another phase split.** A phase profiler can only tell you which of the
     boxes you drew is heavy; only a symbol profiler can tell you the weight is not in
     any of them — and it needs no code, no rebuild and no arm. Corollary: `perf`
     attributes INLINED code to its container, so a hot "function" may be something
     inlined into it (here, an instrument inlined into the present path). Confirm with
     `--sort=sym,srcline` before naming a subsystem.

341. **PRICE THE SETUP AND THE AFTERMATH BEFORE THE PART THAT MADE THE IDEA
     ATTRACTIVE.** Replacing a 128 KB hash with a 256-byte soft-dirty pagemap read is a
     three-orders-of-magnitude argument and it was CORRECT — measured, the kernel's
     answer is 1.6-4x cheaper than reading the bytes for anything above 64 KB. The idea
     is still dead, twice over, on the two costs that are not part of the pitch: ARMING
     it (`/proc/self/clear_refs`) walks the whole process's page tables at 24.4 ns per
     resident page, which on a 1.2 GB resident set is 7.5 ms per frame against the
     ~0.7 ms of hashing it removes; and arming WRITE-PROTECTS every page, so the next
     write to each takes a minor fault at 773 ns/page — charged not to the thread that
     armed it but to whichever thread writes next, which here is the guest's own
     simulation. **Both are invisible in the comparison everyone actually runs**, which
     is "new mechanism per query" against "old mechanism per query". Ask what it costs
     to turn on, and what it costs everybody ELSE afterwards.

342. **WHEN A CACHE KEY IS A PROBE RATHER THAN THE CONTENT, ASK WHAT THE WRONG ANSWER
     *IS*, NOT JUST HOW LIKELY IT IS.** Part 52's plan specified a memo on the shader
     hash keyed by `(address, size)` plus the first and last dword of the microcode —
     "two loads, and it catches the overwhelming majority of a re-upload" — and argued
     the failure mode was benign, because a wrong hash names nothing in the shader cache
     and the standing "no translated shader" gate would therefore catch it. Both halves
     were wrong. The probe was wrong about HALF the times it was consulted for one pair
     of shaders (two different blobs, same size, same first dword, same last dword,
     alternating), because microcode is far too regular for two dwords to identify it:
     dword 0 is a control-flow instruction pair whose encoding repeats across everything
     one compiler emits. And the wrong answer was not a random number that misses — it
     was **another real shader's hash**, which IS in the cache, so the renderer would
     have bound a real, wrong, translated shader and drawn with it, silently, past every
     gate the project owns. **A probe that fails into another VALID key fails
     invisibly.** The fix was to stop probing: compare the whole content with `memcmp`
     against the bytes hashed last time, which is exact and still ~30x cheaper than the
     hash, because the hash's cost was a serial multiply chain and not a memory read.
     Two corollaries. **Write the verify arm even when the argument for the fix sounds
     complete** — this one refuted its own design on its first run, and nothing else
     would have. And **check what your verify arm does to the thing it is standing next
     to**: this one re-inserted into the table on every load and reported 46x more
     evictions than misses until one comparison stopped it (gotcha 7 again, one level in).

343. **BEFORE PRICING AN ITEM OFF A PROFILER PHASE, CHECK WHAT ELSE IS INSIDE THAT
     PHASE — two items priced off two instruments can silently share the same
     milliseconds.** Part 52's plan had item 1.1 (parallel content guards) priced off
     the `GuardFold` SYMBOL at 20-30% of the pump thread, and item 1.4 (parallel command
     recording) priced off the `record` PHASE at 8.69 ms. Both numbers were correct.
     They also overlapped by 39%, because `UploadStream` is called from inside the
     `recordVertex` scope while `ProfScope(streams)` deliberately wraps only the copy —
     so the guard's hash was charged to `record`, and the same milliseconds appeared in
     the price of both items. Splitting it (`record 1,007 ns/draw = state 141 + vertex
     188 + index 161 + GUARD 391 + residual 126`) moved a third of the riskier item's
     apparent size onto the safer one, which reversed which should be built. **A scope is
     a region of code, not a subsystem**, and the check that catches this is cheap:
     reconcile the phase table against the symbol profile and see whether they add up.
     Here they had never been compared, and afterwards they agree to the millisecond —
     391 ns/draw x 6,508 draws = 2.54 ms of stream guard, `GuardFold` reads 4.52 ms by
     `perf`, and the 1.98 ms difference is exactly the texture guard that `textures`
     should contain. Neither instrument could have said this alone: the symbol profile
     knows a function is hot but not which phase pays for it, and the phase table knows a
     phase is hot but not what is nested inside it.

344. **A PARALLEL ITEM'S PRICE IS NOT ITS SYMBOL SHARE — budget the cache it stops
     warming as well as the work it moves.** Part 53 moved both content guards off the
     pump thread onto four workers. `GuardFold` was 25.87% of that thread and fell to
     0.86%; the thread itself fell 63.4% -> 50.3% of a core, so **13.1 points left**. The
     four workers gained **33.2**. Two and a half times as much CPU appeared on the
     workers as left the pump, and neither number is wrong: a memory-latency-bound loop
     is *cheaper interleaved with other work than run on its own*, because on the pump
     its misses overlapped with register decode and driver calls and on a worker there is
     nothing to overlap with. **`perf`-attributed cycles for such a loop understate its
     isolated cost**, which makes a symbol share a good guide to what to MOVE and a bad
     estimate of what moving it will cost. And a second bill lands straight back on the
     thread you were shortening: `recordState` went 140 -> 183 ns/draw and `otherBegin`
     57 -> 81, neither of them code the change touched — the workers evicting the pump's
     working set from the shared L3, ~0.4 ms/frame of it. The item still delivered
     −2.2 ms of a 15.9 ms frame; the point is that three separate things had to be
     budgeted (the work that moves, the dispatch bookkeeping, and the cache), and only
     the first is visible in any instrument that names a subsystem.

345. **A FAST PATH THAT ONLY RUNS WHEN NO INSTRUMENT IS ARMED IS A FAST PATH NOTHING
     TESTS.** Part 53 found the present readback making a 3.5 MB staging copy that only
     the picture instruments read, on a buffer that has been HOST_CACHED for parts. The
     obvious predicate is "copy only if an instrument will read it" — and it would have
     shipped a default configuration whose code path **no gate in the project ever
     runs**, because every picture gate here sets one of those variables
     (`CZ_VK_FRAME_DUMP`, `CZ_CAPTURE_KEY` for the E3 correlation, `CZ_VK_SNAP_ON_*`).
     The predicate that works is the one the instruments do not move: whether the memory
     is cached at all. **Ask what the gates set before choosing the condition on a fast
     path**, or the arm you ship is the arm you never gate.

346. **WHEN A PREDICTION FEEDS A CACHE KEY, THE ONLY SILENT FAILURE IS THE SLOT MIX-UP —
     so make every result carry the descriptor it was computed FOR.** Part 53's guard
     pool hands each cache entry an INDEX into a results array. A stale or wrong index is
     not a miss, it is another buffer's real, valid hash — which compares equal or
     unequal for reasons that have nothing to do with this buffer, and whose symptom is a
     stale mesh with no error anywhere. Exactly the shape gotcha 342 is about, one
     subsystem over. The fix costs two words per slot: the worker echoes back the pointer
     and length it hashed, the consumer checks them, and a disagreement is a loud line
     plus an inline fallback. It has never fired — which is only worth saying because the
     poison arm beside it proves the check can.

347. **WHEN ONE A/B SWITCHES TWO ITEMS TOGETHER, AN ITEM CAN READ NEGATIVE BECAUSE THE
     OTHER ONE TAXES IT — and no number of repeats will fix it.** Part 53's operator
     session controlled both of the part's items with one arm, which is the natural thing
     to do when you want the PART's total and the operator's time is the scarce resource.
     It reported item 1.3 (deleting a redundant 3.5 MB present copy) at **+0.08 ms — the
     wrong direction** — against a headless −0.78, and the item was written up as "did not
     survive the operator's machine". It had survived. Its own arm, with the OTHER item
     held constant, reads **0.668 vs 1.135 ms, −0.467 ms, with no overlap across thirteen
     windows an arm**. Three configurations say why: guard pool OFF + two copies 0.56 ms,
     pool ON + two copies 1.14, pool ON + one copy 0.67. **The parallel guard pool doubles
     the cost of an unrelated `memcpy`**, because four workers streaming ~70 MB/frame leave
     much less memory bandwidth for it — so item 1.3 was saving ~0.47 ms in the very arm
     that made it look negative. The failure is not noise and not sample size: the arms
     were answering a different question from the one being asked. **Give each item an arm
     that holds the others constant, even when a combined arm is what the schedule wants**
     — it cost fifteen minutes here against reverting a change that works. The corollary
     is a pricing rule for everything parallel that follows: a worker pool's BANDWIDTH
     footprint is a cost to other subsystems, and it is big enough to measure.
     **Magnitude corrected the same evening, by the operator, and the lesson is untouched.**
     "The pool DOUBLES the cost" compared their pool-off arm at 40 fps against the per-item
     arm at ~100 fps — two things differing, not one — so it is an inference across
     non-comparable runs. What is matched, and what should have been read first, is the
     per-item arm's own columns: `readback` goes 0.525 -> 0.700 ms with load INSIDE one
     arm, so the saving is a slope (−0.37 ms at 1,900 draws, −0.59 at 4,200) and the −0.467
     mean is the mean of a slope over a range that excluded the operator's load entirely.
     **Two separate reading errors in one section, and both were "quoted the arm mean
     without looking down the column"** — the same failure gotcha 237 is about, one
     instrument over.

348. **A SAMPLE ADDRESSED BY FRAME INDEX IS RE-AIMED BY ANYTHING THAT CHANGES THE FRAME
     RATE.** The E3 picture gate runs a fixed 120 seconds and presses its capture key on a
     fixed schedule, then correlates the five frames it caught against hardware's
     screenshot of the same screen. It read **+0.8808** early in part 53 and **+0.8396** at
     the close, on a code path that had not changed at scale 1 — and the capture filenames
     say why: the first pass sampled frames **1,896-3,853** and the second **5,890-13,874**,
     because the frame cap's default moved 60 -> 500 and far more frames elapse in the same
     wall time. The five presses landed on completely different moments of an ANIMATED
     backdrop. Nothing about what is drawn moved at all. This is gotcha 133 with a specific
     and easily-missed trigger: **a performance change re-aims every picture instrument
     whose trigger is a frame number or a wall clock**, and the drop it produces looks
     exactly like a rendering regression. Check WHICH FRAMES a picture sample caught before
     reading its correlation as a change — the filenames carry it for free — and prefer a
     trigger anchored to an EVENT over one anchored to a count (the same argument that
     produced `WAITJUMP`, gotcha 251).

349. **A SYNTHETIC INPUT EDGE DELIVERED IN A FIXED WALL-CLOCK WINDOW IS A RACE AGAINST A
     POLL RATE YOU DO NOT CONTROL, AND ON A MISS THE WHOLE RECIPE DEGRADES SILENTLY.**
     `CZ_FAKE_PRESS_SEQ`'s host debug edges (`F2`/`F3`/`F4`/`F9`) fired only if the guest
     happened to call `XamInputGetState` inside a **150 ms window at a fixed offset** —
     8.000-8.150 s for the first entry. During a load the guest may not poll for seconds.
     On a miss the edge was LOST: the sequence index walked on, the DebugJump screen was
     never requested, and the route to the outdoor world became "press START a lot" while
     the run continued for its full seven minutes and profiled the prologue. It had been
     winning that race most of the time since part 39, which is why it read as reliable.
     **The transferable shape is the fix, not the bug: an edge should be keyed on the
     INTERVAL BEING REACHED and delivered on the first poll after it, not on a poll
     landing inside a window** — one pulse per entry either way, but a missed window costs
     one poll of latency instead of the whole recipe. And build the arm that forces the
     miss (`CZ_FAKE_PRESS_EDGE_MISS=1`): when the window is hit the new code is
     indistinguishable from the old, so the recovery path is exactly the part no ordinary
     run exercises (gotcha 30).
     **The diagnosis went wrong first, in the standard way.** One run an arm blamed the
     last thing that had changed — the frame-cap default moving 60 -> 500 — and it was
     wrong: three runs an arm read 3/3 and 3/3 six minutes later. **An intermittent failure
     cannot be attributed by a single-run A/B, and the most recent change is the most
     tempting wrong answer** (gotcha 159, and 13 for why the recent change is suspicious in
     the first place).

350. **AN INSTRUMENT THAT READS THE OUTPUT OF THE PATH YOU REPLACED CANNOT GATE THE
     REPLACEMENT.** Every picture gate in this project — `CZ_CAPTURE_KEY`,
     `CZ_VK_FRAME_DUMP`, `CZ_VK_FRAME_STATS`, the E3 correlation — walks the present
     READBACK, the host copy of the frame. Part 54's swapchain arm exists precisely to stop
     making that copy. Run those gates against it and they pass **with the old path still
     doing all the work**, saying nothing whatever about the pixels on screen; run them
     with the readback genuinely gone and they see nothing at all. Either way the gate is
     measuring the wrong thing while looking healthy. **Before gating a change, ask which
     BYTES the gate reads and whether the change produced them.** The replacement here
     (`CZ_VK_SWAPCHAIN_DUMP`) reads back the image actually handed to the presentation
     engine and correlates it against Xenia's own screenshot, so the blit's scale, filter,
     orientation and channel order are all inside the number. Same shape as gotcha 345 one
     step further out: there the danger was a fast path no gate runs, here it is a gate
     that runs on no path the change touches.

351. **A COMPOSITOR SCREEN GRAB READS UNIFORMLY BLACK WHEN THE MONITOR IS ASLEEP.** The
     honest oracle for "does the window show the right picture" is a grab of the actual
     display — something neither the renderer nor its instruments produced. Part 54 built
     that gate, ran it at 01:35, and got five 2560x1440 PNGs whose RGB extrema were
     `(0,0)` on every channel in both arms, scoring `+0.0000` against every orientation.
     Nothing was wrong with the renderer, the grabber or the gate — **and that was
     CHECKED rather than assumed**: the same command run later with the screen in use
     returns a grab with extrema `(0,255)` on every channel. "The monitor was asleep" is a
     comfortable explanation and it had to earn the word "measured". **This is gotcha 231's
     trap one subsystem over** — that one was five sessions of quoting a 210 MHz GPU clock
     sampled with the monitor asleep — and the general form is: **a measurement taken
     through the display is a measurement of the display's power state as much as of your
     program.** Keep the grab as the daytime gate and build one that works without a
     screen; also note the tool question is compositor-specific (KWin does not implement
     the `wlr-screencopy` protocol `grim` needs, and answers with a clear error rather than
     a black image — which is the better failure of the two).

352. **A COST THAT ONLY EXISTS WITH A WINDOW IS INVISIBLE TO EVERY HEADLESS MEASUREMENT,
     AND THIS PROJECT'S PERFORMANCE RUNS ARE ALL HEADLESS.** `Host_PresentPixels` returns
     immediately when there is no window, so the `readback` column of `CZ_VK_PROFILE` has
     read **0.0%** on every headless run in the project's history — including the ones part
     53 used to declare its readback item done. Windowed it is **8.1-8.7% of the frame at
     1280x720 and 16.4-22.6% at 2560x1440**, the largest single non-draw phase at 2x. The
     phase was not lying; it was reporting a path that was not running. **Before pricing an
     item off a profile, ask which of its costs the measurement CONFIGURATION removes** —
     and note the two neighbours this one also hid: the window thread's own copy (8.8% ->
     15.0% of a core between the two resolutions, on a thread no instrument here reads) and
     the GPU's image-to-buffer copy, which shows only as `submit gpu` rising to 14.7%.

353. **A PRESENT-PATH MEASUREMENT HAS TWO RESOLUTIONS, AND NAMING ONLY ONE OF THEM IS
     NAMING NONE.** Part 54 measured a swapchain arm at **−8.3% of the frame at a 720p
     internal render and −31.4% at 1440p** and wrote both down with the internal
     resolution attached, which part 53 had already made the rule. The operator then played
     it and reported *"still feels pretty much the same framerate wise"* — and the campaign
     had left the WINDOW at its default, a 1088x612 drawable, while they play **maximised at
     2560x1417**. Those are not the same experiment, because the two arms do not scale the
     same way with it: **the readback path's CPU cost is a function of the internal
     resolution and is independent of the window, while the swapchain's blit DESTINATION is
     the window.** So the headline number is a number for a small window and the item's
     value at the operator's window size is unmeasured. Two further points. **Their report
     is evidence, not a measurement**: what it is implicitly compared against is a
     remembered frame rate from another day, which is the thing gotcha 51 forbids — it says
     "go and measure", not "the item is worth less". And **no instrument in this project
     records either resolution beside a frame time**, which is why this could be written
     down wrong without anything complaining.
     **THE CAMPAIGN RAN AND THE WORRY WAS REFUTED, WHICH IS THE POINT.** At the operator's
     maximised 2560x1417 window the item reads **−29.0%** against **−31.4%** at 1088x612;
     the window costs the swapchain arm **+2.6…+8.6%** and the readback arm **+2.7…+4.9%**.
     The mechanism was exactly right and the magnitude was small. **Recording it as OPEN
     rather than as "probably fine" was still correct** — a sound argument cannot tell a
     small effect from a large one, and only the measurement could. The lesson survives the
     refutation intact: name both resolutions, and give both arms an instrument that can
     state the one they were measured at.

354. **REPLACING A LIBRARY'S PRESENT MEANS INHERITING JOBS IT WAS DOING INVISIBLY.** Part
     54's swapchain rebuilt only on `VK_ERROR_OUT_OF_DATE_KHR` and merely COUNTED
     `VK_SUBOPTIMAL_KHR`, so a window enlarged after the first present kept being presented
     from the original 1280x720 swapchain and the compositor upscaled it to a 1440p
     monitor. The operator saw it in ten minutes — *"it is blurry"* — and no gate here
     could have, because **no headless gate can resize a window**. SDL's `SDL_RenderCopy`
     had always scaled the full-size texture into whatever the window currently was, so
     resizing had needed no code of ours and appears nowhere in it; the inherited job is
     invisible precisely because nothing we wrote ever mentioned it. **When taking a
     responsibility off a library, enumerate what it was doing that your code never named**
     — window resize, DPI/scale change, output change, minimise — and build the arm that
     forces each (`CZ_WINDOW_RESIZE_AT=SECS:WxH`, which proved the fix in both directions).
     And prefer asking the STATE over trusting a return code: the fix compares the drawable
     size every frame, so it does not depend on which of three plausible compositor
     behaviours this one picks.

355. **AN A/B MEASURES THE LOAD IT SAMPLED, AND A ROAMING CAMPAIGN SAMPLES THE LIGHT END.**
     Part 54's swapchain item was priced, built, gated and A/B'd three rounds an arm at two
     internal resolutions and two window sizes — six campaigns — and every one of them put
     its best-populated draw band at **2,500-2,999 draws**, with the heaviest band having
     more than one window an arm at 3,500-3,999. **The operator plays at 6,700-7,300.**
     Their first soak, both arms in one session, read **−21.1% at ~2,400 draws and −3.5% at
     ~6,800**: the campaigns were right about what they sampled and the headline was a fact
     about the light end of the game. Worse, the shape was wrong too — the write-up said
     this was a fixed per-frame cost whose PERCENTAGE falls with load while the millisecond
     figure holds, and the milliseconds collapse as well, **2.33 ms -> 0.51 ms**. The
     likely mechanism is that at high load the GPU is busy, so CPU time taken off the
     critical thread is absorbed by a longer fence wait instead of becoming frames.
     **This is the third consecutive part in which a SOAK in the heaviest place answered a
     question a roaming campaign could not** (parts 52, 53, 54), and the rule is now
     unambiguous: **take the A/B at the load the player is at.** A roam visits the heavy
     places briefly and the light places for most of its length, so binning by draw count
     does not save you — it produces bands with n=1 exactly where the answer lives. Check
     the WINDOW COUNT of the band you are quoting, not just its delta. And note what saved
     this one: the frame-rate line was made to carry the draw count *for a different reason*
     (the operator's point that spawns differ between runs), and that is what made the
     retraction visible rather than a disagreement about feel.

356. **A COUNTER THAT SAYS A THING WAS DRAWN SAYS NOTHING ABOUT WHETHER THE PICTURE IS
     RIGHT — and a check you write for your own feature will confirm your own feature.**
     Part 54 ported the F4 debug overlay onto a Vulkan present path and instrumented it:
     `swap: debug overlay drawn` read **5,595**, and a dump-based gate confirmed the panel
     was present with 6,466 panel-coloured samples a frame. Both were true. **The entire
     game was black around the panel**, because the overlay was rasterised full-screen with
     transparent margins and a blit is a COPY, not a blend — the zeroed margins overwrote
     the frame. The gate looked for the thing that had been built (is the panel there?) and
     never asked the only question that mattered (**is anything ELSE still there?**). The
     operator saw it in seconds. Two rules follow. **Instrument the NEGATIVE space**: for
     anything drawn over a picture, check the pixels it should NOT have touched, not just
     the ones it should. And **when compositing without a blend stage, the source must be
     exactly the shape of what you want to change** — every transparent pixel in a copied
     image is an opaque black one.
     Two neighbours came out of the same change and are worth the same paragraph. A
     translucent element composited by copy comes out SOLID, and the fix is either a real
     blending pipeline or a CPU blend against a captured background (one frame stale is
     free and unobservable behind a menu). And **alpha must be a parameter of a shared
     layout, never inferred**: the SDL backend recovered the panel's 225/255 by matching
     its colour, which worked exactly as long as there was one backend.

357. **A SHARED STAGING BUFFER WRITTEN AT OFFSET ZERO BY TWO SUBSYSTEMS IS A RACE WITH NO
     ERROR PATH.** The overlay above uploaded through `R->staging` — the renderer's 64 MB
     shared staging buffer — at offset zero, which is exactly where every TEXTURE upload
     writes. A texture upload memcpys its bytes and then RECORDS a `vkCmdCopyBufferToImage`
     that executes later, so anything else writing offset zero before the command buffer
     runs silently substitutes its own bytes. The operator saw one half as "issue appearing
     at the top of debug menu from time to time"; the other half — textures receiving
     overlay bytes — would have presented as an intermittent wrong texture and been chased
     somewhere else entirely. **A staging buffer whose contents must survive until a
     recorded command executes is not scratch memory, and the fix is per-subsystem
     ownership rather than a shared base pointer.** Ask, of any mapped buffer you write:
     what else writes here, and has the GPU finished reading what was here before?

358. **`hardware_concurrency()` COUNTS THREADS, AND A THREAD IS NOT A CORE.** Every
     statement in this project of the form "we use 3.75 of 16 cores, 23% of the machine"
     since part 50 has been counting LOGICAL threads. The operator's machine is a Ryzen 7
     5700 — **8 physical cores, 2 threads per core** — so the real figure is 3.75 of 8,
     **47% of the machine**, and the headroom for a worker pool was half what every plan
     assumed. Two SMT siblings share one core's execution resources: a second thread on a
     busy core buys perhaps 20-30% on a mixed workload and nothing on one already saturating
     the same units, which a memory-latency-bound loop nearly is. **Budget parallel work
     against PHYSICAL cores** (count distinct `(physical id, core id)` pairs — do not divide
     by a threads-per-core constant, which is wrong on any heterogeneous part), and keep the
     logical count only for "how many runnable threads may exist".

359. **ONE THREAD BUDGET FOR THE RUNTIME, NOT ONE POOL PER ITEM — AND SIZE IT FOR THE USER'S
     MACHINE, NOT THE DEVELOPER'S.** The operator's instruction when setting part 55's
     subject: *"even if we really needed the 16 core we should still leave core empty for
     user background item and all. So we should do it smart and depend on amount of core the
     user has instead of aiming for my machine."* Both halves are load-bearing. A plan with
     three parallel items, each sizing its own pool the way the first one did, puts **twelve
     workers** on a six-core machine — plus the critical thread, plus the guest's own busy
     threads, which in this title are two at 70-80% of a core. The budget has to be central,
     shared, and computed from what is ACTUALLY LEFT: physical cores, minus what the process
     already commits, minus a reservation for the OS and whatever the player is running
     alongside the game. **Zero must be a first-class configuration** — on a small machine
     the serial path is the right answer, not a degraded one — and the cap at the top end is
     set by the workload's own serial fraction, not by the core count. Print the chosen
     budget with the machine it was derived from, and state it in every A/B: a parallel
     measurement has a machine as well as a workload, and naming only one is naming none
     (gotcha 353's shape, one dimension over).

360. **A HOT PATH CAN BE TOO HOT TO INSTRUMENT WITH A SCOPE — SPLIT IT WITH `perf` AND THE
     LINE TABLE INSTEAD.** Part 55's largest finding sat behind a lookup taken ~33,000 times
     a frame. A `ProfScope` costs two clock reads at ~20 ns, so instrumenting it would have
     added **1.3 ms a frame** — larger than several of the phases it was meant to separate,
     and an instrument that big does not measure a function, it replaces it (gotcha 7). The
     answer is that a RelWithDebInfo build already carries the DWARF line table, so a flat
     `perf` profile can be folded by SOURCE LINE inside one symbol at zero cost to the
     subject, and at -O2 that attributes INLINED callees to their own lines rather than to
     the container. `tools/part55_srcline.py` is the tool; it is the same move part 51 made
     from phases to symbols, one level finer. **When a phase split is too expensive to take,
     that is a statement about the instrument, not about the question.**

361. **THE BIGGEST COST ON A HOT THREAD CAN BE THE CONTAINER, NOT THE WORK.** Split by source
     line, **89% of `UploadStream` was `std::unordered_map` lookup machinery** — 13.1% of the
     whole pump thread — and `DoDraw`'s two `std::map` shader lookups were another ~4%. None
     of it was visible: the enclosing phases (`streams` reading 0.0%, `otherShader`) NAME the
     lookups and price them together with something else, and the subsystem the cost was
     charged to had been declared closed on the strength of exactly that. `std::unordered_map`
     is chained — every entry a separate `malloc`, so a lookup is a bucket load, a dependent
     chase to an unrelated address and a compare of a key living there, two dependent cache
     misses that cannot overlap — and the standard mandates prime-modulo bucketing, i.e. a
     64-bit division per lookup, with `std::hash` on an integer being the IDENTITY so nothing
     is mixed before it. `std::map` is worse still: a tree walk of ~9 dependent loads.
     **Before parallelising a hot function, check what fraction of it is the container**, and
     remember the two traps in replacing one: with a power-of-two mask the low bits ARE the
     bucket, so a structured key must be passed through a mixer, and the replacement needs a
     verifier because the failure mode of a wrong lookup is a wrong ANSWER, not a crash.

362. **THE ANSWER TO "MAKE IT MULTI-THREADED" CAN BE "DELETE THE WORK".** The operator asked
     part 55 for genuine multithreading and the plan sized three parallel items off a symbol
     budget. The largest single thing on the critical thread turned out not to be
     parallelisable work at all — it was container lookups, and the fix removes them rather
     than moving them. **Strategy (a) — make the work smaller — outranks strategy (b) — move
     the work elsewhere — whenever both are available**, because (b) has a bill that (a) does
     not: part 53 measured 13.1 points leaving the pump while 33.2 appeared on the workers,
     plus ~0.4 ms/frame of cache pollution charged to unrelated phases (gotcha 344). A plan
     that opens with the parallel items should still run the cheap "split it and look" item
     first, precisely because it can retire the expensive ones.

363. **"GEOMETRY BELONGS IN VRAM" IS WRONG FOR A RECOMPILER, AND THE MEASUREMENT IS
     UNAMBIGUOUS.** Modern GPUs expose a `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` heap
     over their whole VRAM (Resizable BAR), so the vertex, index and constant buffers a
     recompiled renderer draws from CAN be put in video memory with a one-line change of
     memory-type preference. On this port that made the frame **~14% LONGER** at the
     operator's soak load — 12.80 ms against 11.18 draw-matched, measured as two three-minute
     soaks in one session with the new arm going first so any first-run penalty landed on it.
     **The reason is that a recompiler is not a normal engine.** A game engine uploads a mesh
     once and draws it for a hundred frames, so the GPU's fetch dominates and VRAM wins. A
     recompiled title re-uploads its shader constants EVERY DRAW, because the guest writes
     its register file every draw: 8 KB per draw here, ~57 MB/frame at 7,000 draws and 90 fps
     — over 5 GB/s of CPU writes — plus ~28 MB/frame of first-touch geometry. Those are
     cached writes at 30+ GB/s in system RAM and write-combined transfers across PCIe at
     roughly a third of that in VRAM. **The GPU saves one fetch; the CPU pays several times
     more to put the data there.** Two corollaries. First, the calculus flips if the
     per-draw upload is removed — so re-ask this question after any change that stops
     re-uploading constants, not before. Second, and unconditionally: **CPU-visible device
     memory is WRITE-COMBINED, so it is write-only.** Sequential writes are fine; a READ is
     an uncached fetch across the bus and can be a hundred times slower than a cached load.
     Audit every read of such a buffer before switching one over — this port found a
     function at 5.31% of its critical thread writing three vertices into the buffer and
     reading all three back, which was free in RAM and would have been ruinous in VRAM with
     nothing naming the cause. Images (textures, render targets, shadow maps) are a different
     case entirely and should be `DEVICE_LOCAL`: they are uploaded through a staging buffer,
     never CPU-mapped, and read many times per frame.

364. **A SESSION-INTERNAL A/B IS ONLY SESSION-INTERNAL IF YOU COMPARE IT AGAINST ITS OWN
     CONTROL ARM.** Part 55 built a chained two-arm soak harness precisely so that both arms
     ran in one sitting on one machine, quoted gotcha 51 in its header, used it correctly
     twice — and then, watching arm 1 live, compared it against **the previous session's**
     control and reported −5.5%. Arm 2 of the same session said **−2 to −3%**, which was also
     what had been predicted before the run. The pull is strongest exactly when the earlier
     number is recent, from the same harness, and at a similar draw count: it looks like a
     control and it is not one. **The rule is mechanical — an arm's number is meaningless
     until its own control arm has run** — and the corollary is that live commentary on a
     running arm should quote the arm's raw numbers and nothing derived, because a delta
     needs a denominator that does not exist yet.

365. **A HYPOTHESIS THAT SPLITS A COST NEEDS A PER-PART COUNTER, OR THE EXPLANATION IS A
     STORY.** Part 55's constant memo failed at 3.6-7.1% as one unit and worked at 32-36%
     once the ALU constant file was split into its two windows, on the hypothesis that the
     VERTEX window is rewritten per draw (a world matrix per object) while the PIXEL window
     sits still. The combined rate is consistent with that explanation and with several
     others; the per-half counter — **VS 2.9%, PS 61.0%** on the operator's machine, VS
     7.4-7.8% / PS 58.4-60.0% on the headless route — is what makes it a finding rather than
     a plausible narrative attached to a number. Cost: three lines. Whenever a split is what
     rescued an item, instrument the split itself, not just the total.

366. **AN ARRAY AND A SEPARATELY-WRITTEN COUNT WILL DRIFT, AND THE FEATURE GOES INERT WITH
     NOTHING REPORTING IT.** Part 56 added `VK_DYNAMIC_STATE_DEPTH_BIAS` to a pipeline's
     dynamic-state array whose `dynamicStateCount` was hardcoded to `3`. The entry was never
     declared, so the pipeline used its static (zero) bias while `vkCmdSetDepthBias` was
     called every draw and ignored — and **nothing reported it**: not the validation layer
     (setting an undeclared dynamic state is not an error), not a gate, not the picture. The
     feature appeared to work only because the driver was lenient, which is worse than not
     working, since it produced a POSITIVE CONTROL that seemed to pass. Derive the count from
     the array (`sizeof(a)/sizeof(a[0])`, or a counter incremented as entries are appended)
     — never write it twice.

367. **READ THE VALIDATION MESSAGE; DO NOT GUESS WHAT THE VUID MEANS.** Part 56 spent two
     build-and-run rounds fixing the OPPOSITE of what `VUID-vkCmdDraw-None-08608` says. It
     was assumed to mean "a declared dynamic state was not set before the draw"; its text
     says the reverse — *"doesn't set up VK_DYNAMIC_STATE_STENCIL_*, but since the
     vkCmdBindPipeline, the related dynamic state commands have been called"*, i.e. calling
     a dynamic-state setter for state a pipeline specifies STATICALLY. The layer prints the
     full sentence and the spec quotation; a VUID number is an index, not a description.

368. **BINDING A PIPELINE THAT SPECIFIES STATE STATICALLY MAKES THE MATCHING DYNAMIC STATE
     UNDEFINED — so a skip-if-unchanged cache cannot survive a bind.** A renderer that
     declares a dynamic state on SOME pipelines only (part 56 declares the stencil states
     just where the stencil test is enabled, which is what keeps the other 82% of draws off
     the requirement) must re-set that state after any bind of a pipeline that does not
     declare it. The symptom is not a wrong picture — it is `VUID-vkCmdDrawIndexed-None-
     0783{7,8,9}`, a draw reading undefined state, which a driver may service in a way that
     looks entirely correct until it does not. State caches keyed only on the VALUE are
     wrong here; the pipeline bind has to invalidate them.

369. **A SIDE EFFECT ON THE RIGHT OF `||` IS A WRITE THAT HAPPENS ONLY WHEN THE LEFT SIDE
     IS FALSE.** The window published its drawable size as
     `if (nw != W.exchange(nw) || nh != H.exchange(nh)) print;` — on the very first
     publish the width exchange returned "changed", the `||` short-circuited, and the
     HEIGHT ATOMIC WAS NEVER WRITTEN, while the print (built from locals) said
     "1280x720". Everything downstream was self-consistent with the lie: the renderer
     read (1280, 0), clamped the zero to the surface minimum, built a 1280x1 swapchain,
     and the compositor stretched it over the window — and the guard that would have
     rebuilt it was itself gated on both values being non-zero, so nothing ever fired.
     The operator's workaround was the diagnosis: a manual resize re-ran the publish
     with the width now EQUAL, which finally let the height write execute. Three rules:
     never put a side-effecting expression after `&&`/`||`; a log line printed from
     LOCALS is not evidence the state was stored (read the atomic back if the line is
     load-bearing); and when a defect's workaround is "poke it once and it stays fixed",
     look for a once-only path that half-ran. Found by reading the stuck process's
     atomics live (gdb x/2wd on the mangled symbols): W=1280, H=0 — one read that ended
     three sessions of deduction.

370. **AN INCREMENT APPLIED IN A PROJECTIVE SPACE IS NOT A SHIFT — where coefficients
     nearly cancel, it is a ROTATION in the Euclidean one.** Part 57's clip-plane bias
     arm added eps·|P| to the plane's w and its ladder was read as "eps=0.01 un-clips
     the whole body, so the body spans <0.01·|P| of margin". Wrong by two orders and in
     KIND: the captured clip planes have c ≈ −d (a unit view plane through Proj⁻ᵀ blows
     both up by ~1/zn, nearly cancelling), so the whole w-increment lands in the VIEW
     plane's z-coefficient — a rotation moving the boundary 0.8–8 METERS at the zombie
     for eps=0.01. The ladder measured the rotation, not any margin. Before reading a
     parameter sweep, transform the parameter into the space where the geometry lives
     and ask what the increment DOES there; a translation by true meters is
     Proj⁻ᵀ·(0,0,0,δ) — two coefficients, not one (`CZ_VK_CLIP_SHIFT`).

371. **A CAPTURED MATRIX IS A SHAPE BEFORE IT IS A VALUE — and the matrix you are
     blocked on may cancel out of the question.** All ten part-57 poses' "biggest draw =
     the scene camera" blocks hold the SHADOW pass's ORTHO matrix (row3 = (0,0,0,1); it
     maps the player inside a unit box — the check costs one dot product), because the
     frame's biggest draw was the ground drawn INTO the shadow map every time. The
     part-58 derivation looked blocked on the missing view matrix and was not:
     dot(P, Proj·v) = dot(Projᵀ·P, v), so the projection ALONE re-expresses a clip
     plane against view space, where a rigid transform of the world makes lengths true
     meters and "is the normal unit?" becomes a space test needing no view matrix at
     all. Verify a captured matrix's structural shape (perspective vs ortho, which row
     is w) before using it — and before declaring data missing, write the question as
     algebra and see what cancels.

372. **A HARDCODED ORIENTATION WITH ZERO CONSUMERS IS A LATENT COIN FLIP — it becomes a
     defect the day a consumer appears, and nothing about the intervening years of
     correct pictures was evidence.** This renderer hardcoded `frontFace = CCW` next to
     `cullMode = NONE`; with culling off and no shader reading gl_FrontFacing, triangle
     facing touched nothing for the whole phase (the title's own su=00080008 on every
     draw meant hardware never culled either). Part 56 gave facing its FIRST consumer —
     two-sided stencil, front REPLACE / back ZERO — and the unverified bit surfaced as
     a view-dependent see-through in sliced zombies two parts later, because the
     complemented stencil mask failed the gore quad's EQUAL test exactly where the cap
     belonged. The pre-part-58 comment even named the interaction ("front-face bit
     interacts with the viewport's Y sign") and deferred it to "when there is a picture
     to check against" — but no ordinary picture can check it: only a facing CONSUMER
     can. When such a bit is finally consumed, treat its value as unmeasured whatever
     the code says, and settle it by experiment (one arm: "it is perfect now"). For
     the record: Xenos FACE=0 + a D3D-convention y-flip on both sides lands on
     Vulkan CLOCKWISE front.

373. **A UNIFORM SCALE FACTOR THAT BECOMES PER-AXIS MUST TRUNCATE, NOT ROUND — because
     floor(a)+floor(b) <= floor(a+b) and round has no such guarantee.** The 21:9 mode
     converts guest X extents by *21/16, and every converted (offset, extent) pair
     must stay inside its converted surface: with truncation that is a theorem, with
     round-half-up it fails on odd widths and the failure is a Vulkan out-of-bounds
     image copy that only a validation run reports. When one number becomes a
     conversion applied in several places, prove the composition property once and
     name it at the definition, or every call site is a separate off-by-one bet. The
     cost of truncation — a right-edge pixel lost on the odd-width tail of a halving
     chain — is bounded and visible; the cost of rounding is UB.

374. **BEFORE BUILDING A COMPENSATION MECHANISM, MEASURE WHETHER THE PRIMARY CHANGE
     ALREADY COMPENSATES.** The 21:9 plan budgeted a HUD-centering mechanism (offset
     window-coordinate draws, with a real risk of catching post-chain quads in the
     same net). Measurement on the first wide dump showed the title draws its
     frontend UI under a 16:9 PERSPECTIVE the projection patch already recognizes —
     so the patch centers the UI as a side effect (x -> 0.5 + (x-0.5)*16/21;
     copyright center 32.8% -> predicted 36.9%, measured 37%, where stretch predicts
     32.8%). The mechanism would have been dead code at best and a post-chain defect
     at worst. Generalization of 362 (the plan's parallel items lost to a simpler
     deletion): run the cheapest arm of the plan BEFORE building its next mechanism.

375. **A KNOB WHOSE UNITS QUANTIZE THE VALUE SILENTLY SERVES A DIFFERENT VALUE — list
     the rungs the units can express before offering a menu of values.** The frame
     cap's period was integer milliseconds; a 90 fps cap needs a 5,555 us period, and
     the ms knob would have delivered 5 ms = 100 fps with no error anywhere. The
     menu's value list (30/60/90/120/240/480) was chosen BEFORE checking which of
     those the machinery could land on — the plan caught it only because the night
     plan pre-registered the trap ("90 would become 100"). When a UI offers discrete
     values, derive each value's actual realization in the mechanism's own units and
     refuse or relabel the ones that do not exist.

376. **vkDeviceWaitIdle DOES NOT COVER THE COMMAND BUFFER YOU ARE STILL RECORDING —
     destroy-then-rebuild mid-frame is UB against your own earlier passes.** The
     snapshot-resize path waited idle and destroyed the old shadow atlas; the frame
     being recorded had already sampled it, and the submit that followed referenced
     destroyed handles — a wedged queue the operator read as a hard freeze, with the
     guest running underneath. The wait-idle also cost a stall PER RESIZE (the
     live-rescale path had already documented that stutter chain "read as a freeze"
     and nobody generalized it). The fix shape is standard and transfers: deferred
     retirement — stamp replaced objects with the current frame and destroy them
     only after every fence that could reference them has been waited (retireFrame +
     framesInFlight + 1 <= frame, drained at the present boundary). Any Vulkan
     resource replaced mid-frame needs this; validation catches the destroyed-handle
     use, but only if a run actually flips the state mid-frame — which is why the
     repro arm (CZ_TEST_TIER_FLIP) exists and why a LIVE-changeable setting must be
     exercised LIVE in its verification, not only per-run (the tier A/B fixed the
     tier per run and could never have found this).

377. **A WRAPPING ORDERED LIST READS AS A RESET when the current value is the last
     entry — and the complaint will describe the OPEN, not the press.** "The menu
     always shows 720p when I open it": three sessions of logs proved the store
     correct at every open; the operator's value was the last list entry, so their
     first right-press wrapped to the smallest — indistinguishable, from the
     player's seat, from the menu opening wrong. Two transferables: ordered ladders
     clamp at their ends (wrap only small cyclic sets), and when a user report
     contradicts the state you can prove, model what their FIRST ACTION would have
     shown them — the report describes an experience, not a state, and the
     open-diagnostic that settled this printed the state at exactly the moment the
     experience began.

378. **RETRACTED IN PART 62, and the retraction is the better lesson: A TWO-RUN
     PICTURE PAIR CANNOT VERIFY A PROJECTION CHANGE — camera drift between
     processes wears a positive result's clothes.** As written, this entry said
     part 61's census (2% of draws carry the recognized projection) was
     misleading because "the frame proved" the +15 arm moved the whole world.
     THE FRAME PROVED NO SUCH THING: the differences between the two runs'
     outdoor frames were camera drift, the world had NOT moved, and the census
     was RIGHT — the 2% was the UI, the world rides a different form (see 380),
     and the operator's first live session refuted the feature in one sentence.
     The original entry even contained the tell it ignored: the same suspicious
     census had to be argued away to keep the picture's verdict. Transferables:
     verify any projection/camera change with a SAME-RUN flip arm
     (CZ_TEST_FOV_FLIP — alternate the state every N frames; the camera holds
     and two dumped frames answer the question), and when a census and a
     picture disagree, the census is per-draw ground truth while a cross-run
     picture is one drifted sample (gotcha 133 in a new costume).

379. **A STATE-BIT SPLIT OF A POPULATION IS NOT A SEMANTIC CLASSIFIER — verify
     what the minority class actually CONTAINS before exempting it.** Part 61
     wanted "UI = ztest-off" so the fov slider could leave the HUD alone; the
     census obliged with a clean three-way depth-state split — and the ztest-off
     class was 81% of recognized draws OUTDOORS, in a frame visibly dominated by
     solid geometry: it holds sky, particles, decals and other scene-space
     content that must move with the camera, plus the few dozen HUD draws.
     Exempting it would have torn effects off the world. The bit answered "which
     draws skip the depth test", not "which draws are UI" — a classifier is only
     as semantic as the thing it reads, and the cheap check is the population
     COUNT against what the eye sees in one frame of that era.

380. **A STRUCTURAL RECOGNIZER VERIFIED IN ONE ERA SILENTLY MISSES THE FORM THE
     REST OF THE GAME USES — census every era AGAINST the recognizer before
     believing its coverage.** Part 60's wide patch recognized the raw 16:9
     projection, was verified on the frontend (attract backdrop: real flank
     geometry), and shipped. The world's draws never carry that form: they carry
     the full view-projection COMPOSITE P*V — so 21:9 GAMEPLAY geometry was
     stretched ~34% for two parts while the menus were perfect, and the fov
     slider built on the same recognizer moved only the UI. The composite was
     recognizable all along (P's structure survives the product: unit view row,
     9/16 row-norm ratio, z-row proportionality) and one CZ_VK_FOV_MISS dump of
     fourteen unmatched matrices found it in minutes. Two transferables: when a
     patch's engagement counter is high but its EFFECT is reported absent,
     suspect TWO populations wearing one name — count per form, not per patch;
     and a verification run must sample the era the feature is FOR (the
     frontend was the era the recipe reached, not the era the operator plays).

381. **A CENSUS THAT PRICES A FEATURE ALSO NAMES ITS OUTLIERS, AND THE OUTLIERS
     ARE THAT FEATURE'S FIRST DEFECT — put the exclusion in the CONSUMER, not in
     the prose.** Part 63's geometry census reported the world inside z ±550 and
     named the exceptions in passing: three streams at ±6.3M units, "junk-
     coordinate effect buffers, not world geometry". Part 64's BLAS collector
     then applied six structural tests (float3 position, sane stride, opaque,
     depth-writing, content-stable, world-form composite) and **every one of them
     passed on those streams**, because each asks what KIND of draw it is and the
     defect is about where its vertices ARE. A triangle spanning millions of units
     is nearer to the sun than the whole town wherever it covers the shadow map,
     which is exactly what the atlas diff measured. The general form: when a
     census calls part of its population junk, that sentence is a specification
     for the code that consumes the census, and leaving it in the document means
     the next part re-discovers it as a picture bug.

382. **"OUR OUTPUT IS NEARER" AND "OUR OUTPUT IS FARTHER" ARE DIFFERENT
     DIAGNOSES — measure BOTH tails, because the one that reads ~zero is what
     eliminates a whole class of explanation.** Part 64's traced shadow map was
     compared against the raster one it replaces: nearer on 49.6% of texels,
     farther on 1.3%. The nearer half alone would have been read as bias or acne
     (and a 33x bias sweep did move the frame, which made that reading look
     right). It was the 1.3% that mattered: a subset missing far occluders MUST
     show a large farther tail, so its absence killed every "we left something
     out" hypothesis in one number and turned the question into "what did we put
     IN". Report a signed difference as two tails and a percentile, never as a
     mean — a mean of ±0.09 and 0.000 is 0.000 and says nothing.

383. **A PICTURE OF A DEPTH BUFFER IS CONTRAST-STRETCHED, SO TWO THAT LOOK
     IDENTICAL CAN DIFFER BY A TENTH OF THE RANGE WHERE IT MATTERS.** The OG and
     traced shadow atlases were visually the same — same trees, poles and
     building silhouettes in all three occupied slices — while half their texels
     disagreed by up to 9% of the depth range. A shadow comparison works at
     ~1e-4; the eye works on the stretch. The dump already prints the 24-bit
     range next to the image precisely so the greys can be converted back, and
     the conversion is four lines. Look to find the surface, then convert to
     judge it (gotcha 133 one instrument along: one frame is one sample, and a
     stretched frame is one sample of the wrong statistic).

384. **A STATS FILE IS BEING WRITTEN WHILE YOU READ IT, AND EVERY STATISTIC IN IT
     DRIFTS WITH RUN DEPTH — quote only runs that have EXITED.** Part 64 read
     `CZ_VK_FRAME_STATS` medians and cumulative log counters from runs still in
     progress, repeatedly, and every one of those numbers was wrong in the same
     direction the route drifts: an arm read at 4,663 outdoor frames scored 63.71
     and the SAME run at 6,484 scored 66.34; another read 72.00 at 1,212 frames
     and 66.40 complete; a cumulative coverage counter read 86.4% early and 52.8%
     at exit. Three "findings" were built on those partials and all three
     dissolved. The route walks through different scenery as it goes, so a
     partial read is not a noisy estimate of the final value, it is a measurement
     of a DIFFERENT PLACE — no amount of averaging fixes it. Gate every read on
     the process having exited (a `done` flag the runner writes, not `pgrep`
     which races the next run), and put the frame COUNT next to every median you
     quote so a reader can see it was complete. This is gotcha 50/51/86 one level
     down: the control is not just the old binary run NOW, it is the old binary
     run TO THE SAME DEPTH.

385. **AN ARM WHOSE MECHANISM IS PROVABLY REAL CAN STILL MOVE THE PICTURE BY
     NOTHING, AND THE MECHANISM'S PROOF IS NOT THE ARM'S PROOF.** Part 64
     established three true facts about this title by direct count — junk
     geometry with million-unit coordinates enters the BLAS; the title's own
     shadow cascade is 52.8% empty because it draws casters not receivers; and
     the cascade pass carries SEVERAL distinct c0-3 matrices per slice, so
     binding the light matrix by recency is false (0 slices with one, 28,704 with
     several). Each was measured, each is a genuine defect, and each produced a
     fix. On complete runs **none of the three moved the frame's brightness
     measurably** — every arm sits at 66.1-66.4 against OG's 80.61. The counts
     prove the mechanisms exist; only an end-to-end measurement proves one
     MATTERS, and the gap between those two claims is where a session quietly
     spends a part. Say "this mechanism is real and its effect is undemonstrated"
     rather than letting the mechanism's evidence stand in for the arm's.

386. **AN ARM THAT READS EXACTLY LIKE ITS CONTROL IS AN INERT ARM UNTIL A COUNTER
     SAYS OTHERWISE — and "exactly" is the tell.** Part 64 combined two RT changes
     (trace the title's own casters; bind the light matrix by dataflow) and the
     result read **80.61 median luma on 11,433 frames against a control's 80.61**,
     which looks like the over-shadowing defect fixed perfectly. Nothing had
     traced: the caster change returned from the world-draw branch before the code
     that populates the dataflow oracle, so no cascade draw was ever vouched, no
     light matrix was ever captured, and not one slice ran. The luma could not
     distinguish "the fix works" from "the feature is switched off", because both
     produce the control's picture EXACTLY. What caught it was the **absence of
     the engagement log line** — a counter that prints only when the feature does
     something, so silence is a reading. Two rules: when a change lands on the
     control's number to the decimal, suspect inertness before success; and a
     feature composed of two changes needs its engagement counter re-checked
     after they are combined, because each was verified alone (gotcha 151 with a
     sharper edge — an arm with no counter cannot be shown to have engaged, and
     an arm that reproduces the control cannot be shown to have engaged EITHER).

387. **A BINDING THAT LIVES IN A REGISTER CANNOT BE READ OFF THE PROGRAM — BUT A
     GPU TRACE IS THE PROGRAM AND THE REGISTERS TOGETHER.** Part 65 needed to know
     which pixel shaders sample the shadow cascade atlas and at which texture
     fetch slot. That is unanswerable from the shader bank: the microcode says
     "fetch slot 3", and WHAT slot 3 holds is a fetch constant the guest writes at
     runtime. The obvious plan was an instrumented run and an operator session. It
     was not needed — a `.xtr` carries hardware's own register file per draw
     alongside the bound shader, so the whole census fell out of twenty capture
     files already on disc, in minutes, with HARDWARE rather than our own renderer
     as the oracle. Before scheduling a run to observe a runtime binding, ask
     whether a trace you already hold contains both halves of it. Generalises
     [[ask-the-whole-capture-set-not-one-capture]]: the captures answer questions
     they were not taken for.

388. **A REGISTER FILE HANDED TO A DRAW IS NOT THE SET OF THINGS THAT DRAW USES.**
     The same census, counted naively by "does any of this draw's 32 texture fetch
     constants point at the atlas", found 768 (shader, slot) pairs. Intersected
     with each shader's OWN declared fetch slots it found 140. The other 628 are
     constants simply left set by an earlier draw — the guest never clears them,
     and there is no reason it should. A per-draw census over a register file must
     be filtered by what the bound program actually READS, or it over-reports by
     whatever the state-leakage rate happens to be (here 6x). Same family as
     gotcha 25 (a grep that cannot match is not a clean result) from the other
     side: a match that cannot mean anything is not a finding.

389. **WHEN A CODE PATTERN VARIES, LOOK FOR THE ALGEBRAIC IDENTITY INSTEAD OF
     ENUMERATING THE PATTERNS.** Route (b) needed the title's own 2x2 PCF weights
     neutralised in 116 shaders so a substituted tap value would survive as a
     continuous number rather than being binarised. The emitted weight products
     came in **thirteen distinct swizzle pairings**, so no pattern match could
     cover them and enumerating them would have been a permanent maintenance
     surface. But every pairing is a product of two components each drawn from
     {a, b, 1-a, 1-b} — so setting a = b = 0.5 makes all four components 0.5 and
     every product 0.25 **whatever the swizzle**. One substitution, provably
     complete, no enumeration. When a transformation has to survive many
     syntactic shapes, find the value that makes them all agree.

390. **A VARIANT SHADER CACHE DRIFTS FROM THE ONE IT WAS FORKED FROM, AND THE
     DRIFT IS SILENT.** This port builds arm caches (`_a2m`, `_clip`,
     `_clip_a2m`, ...) beside the stock one and selects them with an env var. The
     stock cache grew to 449 shaders as operator runs found new ground; the cache
     `tools/play_session.sh` actually selects held **439**, and had since
     2026-08-19. The ten missing are ABSENT rather than stale, so every draw bound
     to one printed `no translated shader` and was SKIPPED — in every operator
     session for three parts. CLAUDE.md already documents the two-line membership
     diff that catches this; it had only ever been run against the STOCK cache.
     Every cache a run can select needs the membership gate, not just the default
     one — and the gate to run is the NAME diff, because the counts can match
     while the sets do not (gotcha 264's shape).

391. **A COUNTER ADDED IN THE SAME COMMIT AS THE THING IT MEASURES IS USUALLY
     NEVER READ.** Part 65 suspected its shadow-factor pass was firing before the
     scene's depth buffer was filled, and added exactly the right instrument:
     `[rtb] the factor pass fires at draw %llu of ~%llu (min %llu, max %llu)`,
     with a sentence in the format string saying what EARLY would mean. It shipped
     in the same commit as four new ladder modes. Every ladder run already on disc
     predated it, the next session's runs were about the ladder, and the number —
     831 of 2,480, which is the answer — appeared in three scratch logs and in no
     document. The hand-off was written around a different theory entirely.
     A counter is read when someone GOES LOOKING for it, and the moment they go
     looking is while the question is live. Add it, then re-run the arm that
     motivated it **in the same sitting**, and put the number in the hand-off even
     when it agrees with what you already believed. See also gotcha 151 (an arm
     with no counter cannot be shown to have engaged) — this is its second half:
     a counter nobody reads is an arm with no counter.

392. **A COUNT OVER A RUN IS NOT AN ORDER WITHIN A FRAME.** A design decision —
     "fire the screen-space pass at the title's own first shadow-sampling draw,
     because by then the Z prepass has filled the depth buffer" — rested on a
     measured fact: 233,155 depth-only draws against 148,150 colour-mode ones over
     a boot. The number was correct and it answered a question nobody had asked.
     Walked in ORDER, the depth-only draws turn out to be the shadow CASCADE
     (a different EDRAM depth surface, its own pitch, edram_mode 5, colour mask 0),
     and the scene pass has no prepass at all: its FIRST draw already samples the
     cascade atlas, with zero depth-writing draws before it and ~5,200 after it.
     Everything downstream — a pass reading a buffer at its clear value — followed
     from that, and three operator sessions and an eleven-rung shader ladder went
     into chasing it as a plumbing defect. **When a design rests on sequence,
     measure sequence.** A histogram cannot refute an ordering claim, and a probe
     inside a shader sees one moment and cannot see order at all — but a GPU trace
     preserves the whole frame's stream, so the question is usually answerable
     offline against hardware in an afternoon (gotcha 387).

393. **A SILENT SAMPLE IS NOT A CLEAR VALUE, AND A CLEAR VALUE IS NOT A BROKEN
     BINDING — but they read identically, so name both before choosing.** A probe
     reporting "the sampled depth is uniformly 1.0" has at least two explanations
     that no amount of looking at the frame separates: the descriptor references
     nothing, or the image really does hold its clear value at that moment. The
     project spent a part on the first because it was the more interesting one,
     and the tell for the second was sitting in its own log. `GetDimensions()` is
     the cheap discriminator when it is genuinely the descriptor (the extent comes
     from the descriptor, not from memory) — but the cheaper move is to ask what
     ELSE would produce exactly the clear value, and to check the timing first.

394. **A ONE-AXIS CONTROL CANNOT DETECT A ONE-AXIS ERROR — SPATIAL CONTROLS COME
     IN PAIRS.** Route (b) computes a screen-space value that patched shaders look
     up at their own position, and the control written to prove that round trip
     carried spatial detail was `frac(uv.x * 8.0)` — eight vertical stripes. It
     landed on the 50/50 midpoint exactly, every time, for three sessions. It was
     quoted as proof the lookup was sound. Meanwhile the factor image was sized
     from a 2048-tall EDRAM attachment while the shaders divided by it and the
     scene was 1440 tall, so every row was read **427 pixels out of place at the
     bottom of the frame** — 30% of the screen. A pattern that varies only in x
     is invariant under a displacement in y; the control could not have failed.
     The operator found it by looking, in one sentence, on the first arm
     ("the shadows move with me and with the camera ... in the form of the
     mountain in the distance"). Add the transposed twin and run BOTH: agreeing
     is the result, either alone is not. Same family as gotcha 30 — a test that
     cannot fail has not been shown capable of failing — but the failure is
     geometric and therefore invisible in the number.

395. **A SILHOUETTE PROVES OCCUPANCY, NOT CONTENT.** A debug mode that returned
     "did the primary ray hit anything" produced a mask whose boundary was a
     perfect skyline: sky read MISS, world read HIT, and the edge undulated over
     the rooftops in exactly the right places when matched against a captured
     frame. That was read as proof the ray-tracing structure contained the world,
     and the investigation moved downstream. It proved nothing of the kind: **a
     bare ground plane plus distant terrain produces an identical silhouette**,
     because a ray aimed at a missing building simply hits the ground behind it.
     The structure turned out to hold essentially only the ground, which is
     consistent with every measurement taken — hits on 85% of the screen, 1.3%
     hemisphere occlusion, and shadow rays escaping. When a mask matches an
     expected outline, ask what ELSE would produce the same outline before
     concluding the contents are right; the cheap follow-up is a probe of the
     thing you actually care about (here, occlusion in directions chosen without
     reference to the light).

396. **AN ARGUMENT LIST THAT ACCEPTS PROSE WILL EVENTUALLY EAT AN ARM — and an
     arm that never ran must not print "done".** A session harness passed every
     argument after the tag through to `env`, human description included. GNU
     `env` treats any argument containing an `=` as an assignment, so
     descriptions written "PASS = ..." were silently absorbed and their arms ran
     perfectly. The first description written without an `=` became the command
     to execute; that arm died in 0.1 s with a 129-byte log, and the harness
     printed "ARM x done." and started the next one. A latent bug in every arm of
     every session, hidden for a whole session by punctuation, and it cost the
     operator two launches. Two fixes, both cheap: prose is its own positional
     parameter and is never forwarded, and the harness refuses to continue when
     an arm's log comes back implausibly small — the same class as gotcha 386, a
     feature that is silently inert reads exactly like a feature that did
     nothing.

397. **READ THE ARTIFACT, NOT ITS EFFECT THROUGH A LOSSY CHANNEL.** Route (b)
     computes a shadow factor into an image of our own, which 126 patched shaders
     then sample and the title's own lighting then applies. Eleven ladder modes,
     three operator sessions and two retracted conclusions went into inferring
     what that image contained by looking at the resulting frame — a channel in
     which the entire range from "fully lit" to "fully shadowed" is about a tenth
     of the frame's median luma (99.9 -> 90.2). A twenty-line readback that
     copies the image into a host buffer and prints mean, shadowed share and
     octiles answered it in ninety seconds, and split the remaining problem
     cleanly in half: mostly lit means the rays are not hitting, mostly shadowed
     means the fault is downstream. **If a feature produces an intermediate you
     own, instrument the intermediate on the day you write it**, and give the
     readback its own positive control (here a poison arm that must read 100%)
     before believing anything it says. Corollary for the dump: record the copy
     into the same command buffer as the pass that produced it — an immediate
     submit photographs the state before the pass has executed.

398. **A TEST OF THE MATRIX IS NOT A TEST OF THE SPACE ITS INPUT IS IN.** Our RT
     collector admits a draw when the constants at c0..c3 have the structure of
     the camera's view-projection, and three parts of documentation read that as
     "so the position stream is world-space". It does not follow, and here it was
     false: c0..c3 is the same view-projection whether the shader feeds it a
     world position or an object position it transformed one line earlier — which
     is what this title's world shaders do, from a row-major 4x3 at vc(8..10).
     Every mesh entered the BLAS in its own local frame with an identity instance
     transform, so a whole town was traced as ~500 meshes stacked at the world
     origin, and every downstream measurement (85% primary-ray hits, a
     perspective-correct world checker, 97.3% of receivers unoccluded) was an
     honest reading of that pile. **A structural test on a transform tells you
     what the transform IS; the space of its input is a property of the SHADER
     and has to be read out of the shader.** The falsifying measurement was one
     line long and could have been run in part 63: transform the stream by the
     matrix the collector approved and ask what fraction lands in the frustum the
     draw was issued into. It read 11.7%.

399. **AN AGGREGATE BOX IS A WEAK ORACLE; PROJECT THE POINTS INSTEAD.** The first
     version of the census reported the accepted geometry's combined world box
     and it looked entirely plausible — 776 x 50 x 854 units, about the size of
     Still Creek. It was a pile of object-space meshes whose local boxes happened
     to sum to that, and the exact ±388.2 / ±427.2 symmetry that gave it away was
     noticed only by accident. A "does this land inside the town" test then
     saturated at 100.0% for every candidate transform and could report nothing
     (gotcha 233's shape again). What discriminated was projecting the actual
     vertices through the actual camera matrix and counting the ones inside the
     clip volume: 0.0% against 61-98%. **When a census must choose between
     hypotheses about WHERE something is, use the projection the hardware itself
     used, not a bounding box and not a plausibility window.**

400. **A "CENSUS FIRST, BUILD NOTHING" INSTRUCTION PAYS FOR ITSELF IN ONE
     AFTERNOON — and the census should re-run the code it is auditing, not
     approximate it.** Part 67's hand-off forbade building before an offline
     census returned a list, after part 66 spent two operator sessions on
     theories. The census that answered it re-implements the runtime collector's
     filter chain *in its own order*, against the vertex bytes in the `.xtr`
     traces, so its buckets are comparable with the runtime's counters line for
     line — and that is what made "our 216..722 instances equal this census's
     count of DISTINCT STREAMS, not of draws" visible. A census that merely
     summarised the traces would have missed it. **Re-run the subject, do not
     summarise it.**

401. **SPEND AN HOUR ON THE PRIOR ART FOR THE PROBLEM CLASS BEFORE THE FOURTH
     SESSION ON IT.** Four parts and eight operator sessions went into a
     ray-traced shadow feature whose central defect — every acceleration-structure
     instance carrying an identity transform because the position streams were
     object-space — is described verbatim in NVIDIA's RTX Remix option list, in an
     option that exists to fix it, together with its symmetrical twin (transforms
     baked INTO the vertices). The same project's source answered three more of our
     open questions in twenty minutes: how to keep an identity for geometry whose
     content changes every frame, how to avoid rebuilding a structure per change,
     and whether to exclude skinned meshes or skin them. **The rule is not "search
     the web first"** — the census-against-hardware discipline is what actually
     found our defect, and no document would have. It is that once a problem is
     recognised as an INSTANCE OF A CLASS someone else has industrialised, an hour
     reading their option names is worth more than another instrumented run. Record
     the licence before reading the source, and take technique rather than code.

402. **A PRICE CHECK ON A CANDIDATE FIX IS WORTH ITS OWN EXPERIMENT, AND ITS BEST
     OUTCOME MAY BE PROVING THE CANDIDATE CANNOT SHIP.** An arm was built to
     exclude a population that was visibly producing a defect. It removed the
     defect and **60% of the world's occluders with it** — because the shader shape
     being excluded was not the actor path it appeared to be, it was the engine's
     main world shader, carrying 2,658 of one frame's 4,512 accepted draws. Had
     that been discovered after building on the exclusion, a session and a design
     would have gone with it. **Ask what a candidate fix COSTS before asking
     whether it works**, and prefer a cost that is measurable from a counter
     (`tlasInst` here) over one that needs an eye.
     Corollary, from the same hour: a cheap discriminator that would have saved the
     expensive fix should be *tested against the census before it is planned around*
     — comparing palette entry 0 with entry 1 to find the draws that do not really
     blend separated nothing, at 63% and 70% over two traces, and the test cost
     five minutes where designing around it would have cost a session.

403. **AN ORACLE THAT WORKED FOR A LARGE ERROR CAN BE SATURATED FOR A SMALL ONE, AND
     IT WILL SAY "NO CHANGE" RATHER THAN "I CANNOT TELL".** The test that proved
     part 67's placement fix — what fraction of a draw's vertices lands in the
     frustum the draw was issued into — went 11.7% -> 98.6% when the defect was
     the whole town piled at the world ORIGIN. Re-used two parts later on a defect
     that misplaces a mesh by METRES, the same test reads **96.55% against 96.38%**
     over a million vertices, and reading that as agreement would have retired a
     real fix on its oracle's ceiling. **Ask what MAGNITUDE of error the oracle
     resolves before reusing it**, and when it saturates, find a statistic whose
     range matches the error: here the draw's own EXTENT (median 1.51 units
     collapsed against 8.75 assembled) and the per-vertex displacement (90th
     percentile 15.5 units). Related to 25 and to "a saturated count measures its
     emitter" — this is the same failure with a continuous statistic.

404. **A RIGID TRANSFORM CANNOT BE WRONG ABOUT SHAPE, ONLY ABOUT PLACE — SO WHEN A
     PER-VERTEX TRANSFORM IS COLLAPSED TO A SINGLE MATRIX, MEASURE THE SIZE, NOT
     THE POSITION.** Collapsing a matrix-palette blend onto entry 0 left the median
     draw's CENTRE within 0.17 world units of the truth while its EXTENT was a
     fifth of it, because a skinned mesh's vertices are stored bone-local and only
     the blend assembles them. Every position-based statistic said "barely moved".
     The general form: when a fix changes how a thing is CONSTRUCTED rather than
     where it is put, the discriminating statistic is a property of the
     construction.

405. **A COUNTER ADDED ONLY TO MAKE A SUSPECTED CASE VISIBLE IS THE ONE THAT FINDS
     THE DESIGN ERROR.** Baking a per-vertex blend into one buffer per mesh has an
     obvious hole — the same mesh drawn twice in one frame under different
     constants — which looked unlikely enough to ship with a counter rather than a
     fix. The counter read **2,364,245 against 4,718,587 placements**: half of
     every such draw. It is the batching mechanism itself, one shared vertex buffer
     with the constant window selecting which props this draw is, and without the
     counter the symptom would have been "five sixths of the world quietly missing
     from the ray structure" with nothing pointing at why. **Write the counter for
     the case you are about to dismiss.** The fix — put the occurrence ordinal
     within the frame into the identity — took `tlasInst` from 682 to 3,356 at a
     matched pass count.

406. **A BUDGET CHECK BELONGS BEFORE THE WORK IT RATIONS, AND THE COUNTER PAIR WILL
     TELL YOU IF IT IS NOT.** A per-frame refit budget was applied AFTER the CPU
     blend that fed it, so two thirds of the blend work was computed and then
     discarded — and the "we re-blended it" counter reported it as if it had
     landed: `rebaked=573807` against `refit=174323`. Two counters for one pipeline
     stage, one before the gate and one after it, make that visible for free;
     one counter would have read "573,807 re-bakes" and been believed.

407. **WHEN EVERY FEATURE SHIPS WITH A CONTROL ARM, THE ARMS ARE A BISECT — AND EACH
     STEP IS ONE RUN RATHER THAN ONE REBUILD.** Four synchronisation VUIDs appeared
     after a part that added four features. Three runs located the cause: the
     PREVIOUS binary (clean, over more frames), the SAME binary with all four arms
     off (clean — so not the unconditional half of the work), and one arm added
     back. A `git bisect` over the same ground is a rebuild per step and cannot
     separate features that landed in one commit. **This is a reason to give every
     feature an arm that has nothing to do with measurement**, and it compounds:
     the arms already existed for the A/Bs.
     The bug itself is worth naming too, because it generalises to any
     build-then-update API: the build batch and the update walk drew from the same
     live set, so a structure whose full build was RECORDED but not executed could
     be issued an in-place update in the same command buffer, reading a source that
     did not exist yet. A GPU fault explains a whole cluster of "object is in use"
     messages at once, because `vkWaitForFences` then returns DEVICE_LOST
     immediately instead of blocking and an ignored return value marks the slot
     retired.

408. **AN ARM WHOSE ENGAGEMENT IS NOT ASSERTED IS NOT AN ARM, AND A SHELL SCRIPT WILL
     SWALLOW IT SILENTLY.** A control arm meant to run `CZ_VK_RT=0` had that string in its
     DESCRIPTION and never in its `env` line, so the "control" ran with the feature fully
     enabled. Had the operator reported "both arms show the defect" it would have been
     read as "not ours" on the strength of a control that was not one — and the run cost
     was already paid. The fix is not care, it is structure: **every arm carries the log
     line that PROVES it engaged, and the harness REFUSES to report an arm whose log does
     not contain it.** This is gotcha 151 ("an arm with no counter cannot be shown to have
     engaged") one level up — 151 is about the runtime counter, 408 is about the harness
     that sets the variable — and it is the second time a shell positional argument has
     eaten an arm's configuration in this project (gotcha 396 is the first, and its warning
     was in the sibling script's own header at the time).

409. **AN INTERMEDIATE THAT LOOKS RIGHT IS WORTH MORE THAN A FINAL IMAGE THAT LOOKS
     WRONG — RENDER THE INTERMEDIATE BEFORE THE FIFTH ROUND OF FIXING THE INPUT.** Four
     parts were spent enriching the geometry a shadow trace could see, because the final
     picture kept showing the same defect and the geometry was the obvious suspect. One
     run of a debug mode that renders WHAT THE RAYS ACTUALLY HIT — already in the shader,
     written two parts earlier — showed a correct depth image of the world, which retired
     the whole suspect class in five minutes. **When a pipeline stage has an instrument
     that images its own output, read it before fixing that stage again**; "the final
     result is still wrong" is evidence about the pipeline, not about the stage you
     happen to be holding.

410. **WHEN TWO ENVIRONMENTS DISAGREE ABOUT A VALUE, CENSUS IT ACROSS EVERY RUN YOU
     ALREADY HAVE BEFORE EXPLAINING IT.** ~~A latched sun direction read
     `(-0.364 0.546 -0.755)` in every windowed run and `(-0.371 0.557 +0.743)` in every
     headless one~~ **(RETRACTED IN PART 70 — see 411; the partition was not
     windowed-vs-headless and the values were not in conflict).** The advice stands and
     the example does not: census before explaining. A latched sun direction read
     `(-0.364 0.546 -0.755)` in one group of runs and `(-0.371 0.557 +0.743)` in another — two components agreeing to 2% while the third flips sign, which no
     day/night cycle produces. It cost one `grep` over logs already on disk, and it
     matters out of proportion to its size: every offline measurement the feature had ever
     made was taken in the mirrored configuration. Related to 50/51/86 (a rate measured
     once is a fact about that afternoon) — the general form is that HEADLESS AND WINDOWED
     ARE TWO DIFFERENT ORACLES and any value latched from the scene should be diffed
     between them before either is trusted.

411. **AN ARM LABEL NAMES ONE DIFFERENCE; CHECK THAT IT IS THE ONLY ONE — AND CHECK IT IN
     THE LOGS, NOT IN YOUR MEMORY OF HOW THE RUNS WERE MADE.** Gotcha 410 partitioned a
     disputed sun vector as "windowed" against "headless" and made it a feature's live
     lead. The label was the confound: re-reading all 36 archived logs, **zero contain a
     `requested DebugJump` line and zero contain synthetic input** — every one is an
     operator run that loaded THEIR save, while every run on the other side of the
     partition reaches the world through the DebugJump screen and spawns at a fixed story
     point. The two groups differed in the PLACE and the STORY TIME as well as in the
     window, and the game has a day cycle, so both values could be right — **and a
     destination sweep on the headless harness then latched the "windowed-only" vector
     exactly, which turned "could be" into "is"**. This is the
     project's A/B admissibility rule (two configurations are comparable only if they are
     two states of ONE thing) applied to a census rather than to a picture, and the check
     that would have caught it is two `grep -c` over logs already on disk. The corollary
     bites harder than the rule: **a group label attached after the fact is a hypothesis
     about what the runs had in common, and it is testable.**

412. **THE TITLE OFTEN STATES, AS A CONSTANT, THE QUANTITY YOU ARE DERIVING FROM ITS
     MATRICES — LOOK FOR THE LABEL BEFORE BUILDING THE DECOMPOSITION.** Five parts of an
     RT shadow feature recovered the sun's direction by capturing a cascade matrix,
     choosing among candidates with a per-frame majority VOTE, inverting it, unprojecting
     two clip points and negating the difference; three separate attempts were needed just
     to pick the right matrix. The title uploads a unit sun direction at pixel constant
     `c23` and lights from it, and that constant had ALREADY BEEN PRINTED in this project's
     own notes two years of parts earlier — in a table built to answer a question about
     exposure, annotated only as "a unit direction". A derived quantity carries every
     ambiguity of the derivation (which matrix, which sign convention, which handedness);
     a constant the guest uploads carries none. **When a capture tool can dump the whole
     constant file, dump it and read what is in there before writing arithmetic**, and
     when you do use the constant, bind it two-sidedly — here the same block's cascade
     matrix agrees with it to 0.00 degrees on hardware, which makes "these two disagree"
     a reportable defect rather than a silent wrong answer.

413. **A CENSUS OF A VALUE OVER A RUN NEEDS A TIME AXIS, OR IT CANNOT TELL "IT MOVED"
     FROM "THE SELECTION FLIPPED".** A run's sun census printed three directions with
     counts and no ordering. Two readings of that table are possible and they point at
     opposite subsystems: a light that genuinely moved owns a contiguous stretch of
     frames, while a selection that flips interleaves with the others. A part was spent on
     the second reading. First and last frame per cluster is two `uint64_t` and one line
     of `printf`. This is gotcha 405 ("a count over a run is not an ORDER") in the
     instrument rather than in the evidence — and the reason it recurs is that a counter
     is the cheapest thing to add and the ordering is only wanted later.

414. **AN ARM NAMED AFTER A FEATURE BOUNDS THAT FEATURE'S COST ONLY IF EVERY PIECE OF THE
     COST IS GATED ON THE FEATURE.** `docs/perf-plan-part71.md` §1.4 named `CZ_VK_RT=0`,
     "the whole RT device off", as the crude bound on parts 59-70's per-draw hooks. It is
     not one. The largest piece of that cost — a full texture-fetch-constant decode per
     declared fetch of all 126 RT-variant shaders, 9,482,873 of them in a 100-second run
     of the shipped build — was gated on `ps.moduleRt`, i.e. on whether the RT variant
     SHADER CACHE loaded, which the loader does with no reference to `rtEnabled`. The arm
     would have measured approximately nothing and its null would have read as "the hooks
     are free". **Before trusting an arm, follow the cost to its guard and check that the
     guard is the arm's variable** — a piece gated on an ASSET the feature loads outlives
     the feature being switched off. Sibling of 408 (an arm that never engaged) and of 25
     (a grep that cannot match): all three are instruments that report a clean zero
     because they were never able to report anything else.

415. **AN ARM THAT CHANGES A SECOND THING IS A DIFFERENT EXPERIMENT, AND THE SECOND THING
     IS USUALLY THE BIGGER ONE.** `CZ_VK_WIDE=0` was the plan's arm for part 62's
     wide-culling over-widen. On this operator's 3440x1440 setting it also forces the
     internal resolution to 2560x1440 — **26% fewer pixels** — so its frame-time delta
     would have been mostly GPU and would have been reported as a CPU culling saving.
     `CZ_NO_GAME_FOV=1` removes the same over-widen at a constant pixel count. **Check
     what an arm does on the CONFIGURATION IT WILL RUN ON, not in general**: the same
     variable is a clean arm at 16:9 and a confounded one at 21:9, and the plan was
     written before anyone read `cz_settings.txt`. Gotcha 349's "a present measurement has
     two resolutions" is the same defect one layer down.

416. **"MEASURE BEFORE BUILDING" AND "BUILD THEN MEASURE" COST THE SAME WHEN EVERY RUN
     BELONGS TO SOMEONE ELSE — SO BUILD THE ARM.** Part 71's plan asked for
     `tools/part55_srcline.py` on the per-draw hook chain before writing the fold. But
     `srcline` needs a run of the game and the standing instruction is that the operator
     drives every run, so profiling first and shipping-with-an-arm first both cost exactly
     one operator session — and only the second one PRICES the item instead of sizing it.
     The precondition is that the change be behaviour-preserving *by construction* (here
     the fold's word is the OR of every hook's own arm, so it cannot be false while any
     hook could do work), which is what the "measure first" rule was really protecting.
     **Pre-registering the kill threshold is what keeps this honest** — under 0.3 ms and
     it comes back out — and it is the half people drop.

417. **A DRAW BAND MUST BE NARROW RELATIVE TO (EFFECT SIZE / SLOPE), NOT MERELY
     "NARROW".** Part 71's four-arm soak was read first with `part54_fps_bins.py --band
     500`, which put one arm's 9,774-9,862-draw windows in the same bin as the other's
     9,480-9,585 ones. At this title's ~2.5 us/draw a 300-draw mismatch inside a bin is
     0.75 ms — the entire size of the effect being measured — and the result came out
     +0.25 ms for an item whose pre-registered kill threshold was 0.3 (it would have been
     dropped) and +0.49 ms for one that actually costs +0.09 (it would have been chased).
     Re-read at the band where both arms genuinely overlap, both flipped. **The check that
     costs nothing: print each arm's within-band DRAW MEDIAN beside its frame time.** If
     they differ by more than effect/slave slope, the bin is not a comparison. This is the
     "two soaks are never at the same draw count" rule (`perf-state-parked.md` §3) failing
     at a level nobody was watching — inside a bin that was already supposed to have fixed
     it.

418. **AN INSTRUMENT GATED BEHIND AN EXPENSIVE ONE IS OFF EXACTLY WHEN IT IS NEEDED.**
     Pipeline creation had a timer for several parts. It was gated on `CZ_VK_PROFILE`,
     which costs 2-4 ms a frame — so it was off in every operator session, which is to say
     off in every session whose stutter anyone ever reported, and the hypothesis it
     existed to test was inferred three times and measured zero times. The fix is to ask
     what the instrument actually COSTS rather than which bucket it belongs in: two clock
     reads per pipeline and ~500 pipelines a run is ~20 microseconds in five minutes, so
     it never needed a gate at all. **Rate, not category, decides whether an instrument
     can be unconditional** — the same arithmetic that says a `ProfScope` cannot go on a
     33,000-call-a-frame path (gotcha 360) says a timer CAN go on a 500-call-a-run one.

419. **THE OPERATING POINT MOVES, NOT JUST THE CODE.** Part 71 re-baselined because
     thirteen parts had shipped since the last measurement. The re-baseline found the
     frame at 28.1 ms rather than the expected ~12 — and the cause was not thirteen parts
     of regression, it was that the operator had moved to 3440x1440 and a heavier spot:
     **5.4x the pixels and 1.4x the draws the old number was taken at.** Every stored item
     estimate was priced against a workload that no longer existed. **A baseline has two
     halves that go stale independently — the binary and the load — and a plan that only
     re-runs the binary has re-baselined half of it.** Print the resolution, the draw
     count and the settings file with every number (this session's harness now echoes
     `cz_settings.txt` in its preflight), because those are what let a future reader tell
     which half moved.

420. **BEFORE OPTIMISING A RENDERER, CHECK WHETHER IT PASSES `VK_NULL_HANDLE` AS THE
     PIPELINE CACHE.**
     **[PART 72 RETRACTS THE FIX, NOT THE COST.** The 17,827 ms is real and it is what a
     genuinely cold shader set costs. A persisted *application* `VkPipelineCache` is NOT
     what removed it: run the arms backwards and `nocache` — no cache object at all — is
     the cheapest of the seven runs ever taken, 0.105 ms/pipeline against `warm`'s 0.786.
     A **driver-side** cache that survives process exit is the mechanism. The lesson below
     stands with its subject changed: grep for `VK_NULL_HANDLE` on day one, and then find
     out whether the driver is already caching for you before you credit your own file.
     **See gotcha 426.]**
     This one did, at all three creation sites, from the day the renderer
     was written until part 71 — and it cost **17,827 ms of pipeline compilation in a
     five-minute session**, on the pump thread, including a single **3,753.9 ms frame that
     built 97 pipelines**. That is not a tuning opportunity, it is a missing line of
     bring-up code, and it outweighed every per-draw item the project had spent six parts
     on. It hid for one reason: it is not a per-frame cost, so a profiler that reports
     *rates* cannot see it and a *median* frame time cannot see it — it is a handful of
     enormous frames in the first minute. **The class is "one-time costs on the critical
     thread", and the instrument for it is a per-EVENT roll-up, not a per-second one.**
     Anyone porting a title with this pipeline should check it on day one; it is free.

421. **THE OPERATOR'S EYE SATURATES, AND THE SATURATION POINT IS DATA.** They separated a
     17.8-second compilation bill from a 1.2-second one instantly and unprompted, and could
     not rank 1.16 s / 0.45 s / 0.48 s ("maybe 3 slightly less, not sure"). Both facts are
     useful and they point opposite ways: above the perception floor a human verdict
     outranks any statistic (that is why `the-operator-eye-answers-shape-questions` exists),
     and below it the instrument outranks the verdict and asking for a felt A/B wastes a
     session. **Write down where the floor was**, or the next part asks a human to
     adjudicate something they cannot see and reads "no difference" as "no effect".

422. **A THREE-STEP IMPROVEMENT NEEDS A RE-ORDERED ARM TO ATTRIBUTE ITS MIDDLE STEP.**
     Pipeline compilation went 17,827 -> 1,160 -> 451 ms across arms run in that order, and
     the big step happened while the feature's own file was still EMPTY (`0 bytes seeded`) —
     so it is not the file. Two mechanisms fit (the cache OBJECT enabling intra-run reuse,
     or a driver-side implicit cache that the first arm warmed) and a session run in one
     order cannot separate them, because arm order and cache state are confounded by
     construction. **The fix costs one run: put the control arm LAST.** Generally — when
     arms warm something shared, the arm order IS a variable, and the cheapest way to prove
     it is not the variable is to run them backwards.

423. **AN ARM THAT REMOVES THE DEFECT *AND* THE FIX PRICES NEITHER.** Part 71 measured the
     21:9 culling over-widen with `CZ_NO_GAME_FOV=1` and read +1,930 draws of 9,817, which
     the plan carried forward as "≈4.8 ms of 28 — the best-priced item we have". But that
     arm turns off the *whole* fov substitution, and the substitution does two things: it
     widens the frustum horizontally (the part-62 fix, which is being KEPT) and vertically
     (the waste, which is the item). The configuration the item would actually reach has
     the arm's vertical and today's horizontal, so its frustum is a strict SUBSET of
     today's and a strict SUPERSET of the arm's — **the recoverable draws are strictly
     fewer than the arm's difference**, by containment and with no model needed. Two models
     put it near half. **Before quoting an arm difference as an item's value, ask what ELSE
     that arm turned off**; the answer is almost never nothing, and here it roughly halved
     the number that had put the item first in the plan. Same family as gotcha 415
     (`CZ_VK_WIDE=0` also dropping 26% of the pixels) and it bit the very next part.

424. **A LIVE PROPERTY TRACE CANNOT PROVE A PROPERTY DOES NOT EXIST.** The question was
     whether this engine's camera carries an aspect ratio as well as a fov; the instrument
     to hand was `CZ_FOV_PROP_TRACE=1`, which prints named-property registrations as a run
     constructs them. A null from it would have been a fact about the route taken, not
     about the game — the boot path constructs a fraction of the classes. **The scan over
     the image answers it once and for everything**: walk `.text` for calls to the
     universal binders and reconstruct each site's name pointer from the preceding
     `lis`/`addi` chain (`tools/find_named_properties.py`, 2,056 sites, 1,966 names).
     Exactly one `Aspect` in the whole game, on a 2D spawn box, against sixty-odd camera
     configs registering `FOV`. **Print the RECOVERY RATE with the answer** (95.6% here) —
     a null from a scanner that could only read 40% of its sites is not a null, and this is
     gotcha 3 in the shape it takes when the detector is your own.

425. **A GEOMETRIC PREDICATE INSIDE A HUGE TRANSLATION UNIT STILL DESERVES A UNIT TEST.**
     The vertical-waste census is fifteen lines of "project eight corners and compare
     against ±w". A sign error in it does not crash, does not look wrong, and reports a
     plausible number that no operator session can audit — the worst failure shape this
     project has, because it consumes a session and produces a confident wrong decision.
     There was no seam to link against, so the arithmetic was copied verbatim into
     `tools/vcull_predicate_test.cpp` with thirteen boxes whose classification follows from
     the projection's own geometry. **The duplication is the price of having a gate at
     all**, and it is worth paying: the test caught a wrong expectation on its first run and
     screams (0 -> 6 failures) when one comparison's sign is flipped (gotcha 30).

426. **THE DRIVER MAY ALREADY BE CACHING FOR YOU, AND CREDITING YOUR OWN CACHE FOR ITS WORK
     IS A ONE-ORDERING MISTAKE.**
     **[THE SECOND HALF OF THIS ENTRY WAS ITSELF A ONE-ORDERING MISTAKE — see gotcha 433.
     "With it warm, passing `VK_NULL_HANDLE` was the *fastest* of the seven" did not
     survive the run that put `nocache` first. The first half stands.]** Part 71 measured pipeline compilation at
     17,827 -> 1,161 -> 451 ms across `nocache, cold, warm` and read it as "our persisted
     `VkPipelineCache` is worth −97.5%". Part 72 ran the same three arms as
     `cold, warm, nocache`. The **pipeline COUNT held at 484-513 across all seven runs**,
     which controls for the route, so ms/pipeline is comparable — and the same arm in two
     positions says it all: `cold` 2.398 -> 1.676 (1.4x), `warm` 0.920 -> 0.786 (1.2x),
     **`nocache` 36.457 -> 0.105 (346x)**. If POSITION were the variable, all three would
     move. Only the one with no cache object did, so what changed is **outside the
     process**: a driver-side cache that persists across runs. With it warm, passing
     `VK_NULL_HANDLE` was the *fastest* of the seven. **Two rules.** Before crediting an
     application cache, run the no-cache arm LAST — gotcha 422 said run them backwards and
     this is what that buys. And find an invariant that makes the arms comparable (here,
     the pipeline count) or the comparison is between routes, not configurations.

427. **A CONTROL YOU ADD BECAUSE IT "SHOULD BE BORING" IS WORTH MORE THAN THE HEADLINE IT
     ACCOMPANIES.** Part 72's vertical-waste census printed a horizontal figure purely as a
     sanity control, with the expectation written into the format string: the horizontal
     widening is kept, so the number should be small. It read **98.1%**, leaving ~142
     on-screen draws to paint a scene submitting 9,750. That is the only reason a wrong
     number was not published as the plan's biggest item's price — the headline itself
     looked perfectly reasonable (62 draws/frame, falling to 1 under the semantic control).
     **Give every instrument a channel whose value you can predict**, and make the
     instrument REFUSE to report when that channel is wrong rather than leaving it to a
     reader to notice.

428. **A CUMULATIVE MEAN PRINTED EVERY N FRAMES LOOKS LIKE A TIME SERIES AND IS NOT ONE.**
     The same census reported 184 -> 188 -> 157 -> 135 -> 118 -> ... -> 62 draws/frame and
     it reads like a quantity settling down. Multiply back out and the TOTAL is flat from
     frame 3,000: every one of those draws was accrued in a 1,200-frame burst during the
     approach to the soak, and the steady state — the thing being measured — was **1.0 a
     frame**. The decay is `C/n`. **Print the delta since the previous line**, and when
     reading someone else's periodic average, multiply by the count and difference it
     before believing the trend. Gotcha 237 is the same defect wearing a frame timer.

429. **A RETRACTION FILED SOMEWHERE ELSE LEAVES THE ORIGINAL CLAIM QUOTABLE.** §6cs
     concluded that composite-draw position streams are world-space. Part 67 refuted it
     with 46,820 draws (boxes intersecting their own frustum: 0.1% untransformed, 97.8%
     placed) and recorded the refutation in a CODE COMMENT and in §6cy — but not in §6cs.
     Part 72 then read §6cs, cited it by name, and built a census on it that reproduced
     part 67's 0.1% figure exactly. **Re-reading the source would not have caught it**, and
     the "retract in place" rule this project already has is precisely the defence: the
     correction has to land where the claim is, not only where the correcting work
     happened. When a retraction spans documents, edit the ORIGINAL first.

430. **A NEW FAST PATH CAN SILENTLY INVALIDATE THE VERIFIER OF THE OLD ONE, AND THE
     VERIFIER IS THE FIRST THING SOMEONE REACHES FOR WHEN THE FAST PATH LOOKS WRONG.**
     Part 72's constant GATHER copies only the ALU registers a shader reads. The constant
     MEMO's verifier, written when every copy wrote the whole 256-register window, compared
     all 1,024 dwords against a recomputed full copy — so with the gather on it would have
     reported **the feature working** as a memo defect, on every draw. Two instruments, one
     of them quietly turned into a false-alarm generator by the other, and the false alarm
     lands in the exact tool an investigator would open first. The fix is small (compare
     only the shader's own list when gathering, the whole window when not); the lesson is
     that **shipping a fast path means auditing every existing check that assumed the slow
     one**. Its POISON arm had the same defect one level down — it corrupted register 0
     unconditionally, which under the gather is not necessarily a register anyone reads, so
     the positive control would have gone blind while reporting the verifier healthy.

431. **WHEN A CACHE KEY OMITS SOMETHING THE OLD PATH DID NOT CARE ABOUT, A NEW PATH CAN
     MAKE IT LOAD-BEARING.** This renderer's constant memo keys on (constant version,
     window base) and NOT on the shader — correct and cheap for eighteen parts, because a
     full window copy serves any shader equally. The gather broke that silently: a slot
     holding shader A's nine registers, served to shader B on a version match, hands B
     whatever the bump arena left in the registers only B reads. Nothing crashes and one
     shader is subtly wrong. **Before adding a path that makes a cached artifact
     SHADER-SPECIFIC, re-read what the cache's key does and does not contain.** The fix
     here was to have the slot remember its occupant and re-gather on a change — cheap
     because the version match guarantees the source registers are unchanged, so rewriting
     them is idempotent.

432. **A LIST THAT DECIDES WHAT NOT TO COPY CANNOT BE CHECKED AT RUN TIME.** The gather's
     per-shader register list is load-bearing for correctness: a register it omits is never
     copied, so the shader reads arena garbage. The run-time verifier can only confirm that
     the gather copied what the list NAMES — it cannot confirm the list names everything
     the shader reads, because the missing register is by definition the one nobody looks
     at. **That check has to be offline and against the shader itself**
     (`tools/alu_const_gate.py`, re-parsing the HLSL rather than trusting the writer that
     produced the list). Generalises to every "skip the work we know is unnecessary"
     optimisation: the knowledge is an oracle, and an oracle needs its own gate.

433. **TWO ORDERINGS ARE NOT ENOUGH WHEN "LATER IS CHEAPER" IS ITSELF THE EFFECT.** Part 71
     ran `nocache, cold, warm` and concluded its pipeline cache was worth −97.5%. Part 72
     ran `cold, warm, nocache`, saw `nocache` come out cheapest, and concluded the opposite
     — that the cache was a 7.5x *pessimization*. Then it ran `nocache, warm` and got the
     opposite again: warm 5.8x cheaper. **In every session the arm that ran LATER won, and
     the arms swapped roles between sessions.** The variable was never the arm; it was
     cumulative warming of a driver-side cache that persists across processes AND across
     sessions, so each run is cheaper than the last whatever it is configured as.
     **Reversing the order (gotcha 422) proves the first conclusion wrong; it does not
     prove the second one right.** When the confound is monotone in time, the fix is not
     another ordering — it is to interleave the arms repeatedly within one session, or to
     accept the question is unanswerable on a warmed machine and say so. The absolute cost
     here fell 17,827 -> 59 ms across one day, which is the real finding: the only run that
     was ever expensive is the first one anybody ever does.

434. **PRE-REGISTER THE THRESHOLD ON THE STATISTIC THE DECISION TURNS ON, NOT THE ONE THAT
     IS EASY TO COLLECT.** Part 72's plan gated an item on a cache's SKIP RATE — "kill it
     if above 90%" — and the counter came back at 83.5%, so the rule said keep it. Pricing
     it properly killed it instantly: the population that cache serves is **6.34% of
     draws**, so the whole item is 306 driver calls a frame, **0.006-0.031 ms of a 26 ms
     frame**. Even a cache serving zero percent would have cost ~0.19 ms. The rate was
     collected because the counter already existed; the decision always turned on the
     absolute call count, which needed one multiplication nobody did. **A pre-registered
     threshold is only as good as its statistic**, and the tell is that the rate has no
     units the frame budget recognises. Sibling of gotcha 237 (a mean frame time measures
     the pacing floor, not your change) and of `measure-the-shape-not-just-the-share`.

435. **A COUNTER NAMED FOR THE FUNCTION IT SITS IN MEASURES THE CALL, NOT THE WORK THAT
     FUNCTION USUALLY DECLINES TO DO.** Part 72 shipped a slow-frame table with a "texture
     uploads" column; it differenced a counter incremented at the top of `UploadTexture`,
     which is entered once per texture fetch per DRAW and uploads on well under one call in
     a thousand. Its first run read **2.24-2.43 "uploads" per draw in every one of twelve
     rows** — the column was the draw count in another unit, carrying no independent
     information, and it would have been read as "texture uploads track the slow frames"
     because it correlates perfectly with everything else that scales with draws. The tell
     is available for free: **divide the column by the one next to it.** A per-frame count
     that is a fixed multiple of another column is not a second measurement. Fixed by
     counting, weighing AND timing at the site that actually uploads — the run total is
     ~2,350 where the old column reached 15,233 in a single frame. Sibling of gotcha 171
     (an early return shadows a counter) one level up: here the counter was placed BEFORE
     every early return on purpose, which is correct for a denominator and wrong for the
     thing being denominated.

436. **A CHANGE WHOSE MOTIVATING HYPOTHESIS IS REFUTED DOES NOT GET TO STAY ON A DELTA YOU
     CANNOT MEASURE.** Part 73 measured 96.7% of the texture-upload cost inside
     `vkQueueSubmit`+`vkQueueWaitIdle` and inferred the obvious mechanism: `vkQueueWaitIdle`
     waits for the WHOLE queue, so during frame recording every upload serializes the pump
     against the in-flight frame. A fence on the single submit should therefore be much
     cheaper. It measured **3.2x WORSE** (792 us against 258). One more run isolated the
     cause — the per-call `vkCreateFence`/`vkDestroyFence` sat inside the timed window — and
     with a persistent fence the two primitives agreed to **6%**, i.e. under what the route
     can resolve without a null arm. So the wait primitive was never the cost; ~250 us is
     simply what one submit round-trip costs, and only *not doing 2,350 of them* removes it.
     **The revert is the point.** A 6% improvement is a fine reason to keep a change when
     the reasoning behind it holds; when the reasoning is dead, the same 6% is
     indistinguishable from noise and the change is complexity on a hot path with nothing
     behind it. Two lessons ride along: **put the setup you are not interested in OUTSIDE
     the timed window** (the first run's headline was 68% object lifecycle), and **a
     surprising A/B result deserves one discriminating run before it becomes a conclusion**
     — the first number said "fences are terrible here", which is false.

437. **A CONTROL ARM THAT MOVES THE WRONG WAY IS A RESULT, NOT A BROKEN ARM — AND IT CAN
     REFUTE THE ITEM THE MEASUREMENT WAS BUILT TO PRICE.** Part 73's census put the
     wide-culling item's vertical waste at 0-36 draws/frame against a pre-registered kill of
     700, which alone is only a null. The plan's own semantic control was
     `CZ_NO_GAME_FOV=1`, with the written expectation that *"the vertical waste must fall
     sharply."* **It rose from 0-36 to 270-329 draws/frame** and the on-screen share fell
     99.4% -> 50.5%. Nothing was broken: that arm narrows the PROJECTION while the game
     still submits for its own frustum, so half the submitted draws stop landing on screen.
     Read together, the two arms say the widening is **not drawing invisible geometry — it
     is what makes the geometry the game already submits visible**, so the item's premise
     was wrong rather than small. The item had been priced at 4.8 ms, then 2.5-2.8 ms by
     containment, then declared unpriced; none of those were measurements. **When you write
     a control's expected direction into a plan, you have made a prediction about the
     mechanism — check the direction as carefully as the magnitude, because the direction is
     where the mechanism lives.** Sibling of `an-arm-label-is-a-hypothesis`.

438. **A FIXED-INTERVAL INPUT SCRIPT LEAVES THE CAMERA PARKED FOR THE REST OF THE RUN, AND
     EVERY RATE AVERAGED OVER THAT TAIL IS A FACT ABOUT ONE POSE.** `tools/autoroute.sh`
     builds its press sequence from 8-second intervals and then runs to a much longer
     timeout, so a 90-second turn block inside a 450-second run leaves ~60% of the frames
     showing one frozen view. It is visible in any windowed statistic as *identical to the
     decimal* across consecutive windows — the part-73 census read `scene draws 4,313 /
     classified 804 / on screen 798 / horizontal 6.0` four windows running. **Discard the
     frozen tail before quoting a rate**, and prefer statistics printed per window over run
     means, which the tail dominates by weight of frames. This is the stationary cousin of
     gotcha 428 (a cumulative mean over a transient) and it bites in the opposite
     direction: there the early burst dominated, here the late stasis does.

439. **AN OLD BINARY IS NEVER A SINGLE-VARIABLE ARM.** Part 74 built the pre-RT binary to
     answer *"how does the game fare against before we added all the RT stuff"* — the right
     instinct, and the right method (the control is the old binary run NOW, not its
     remembered numbers). It measured **45% faster**, which read as thirteen parts of
     regression. It was not. The old build's own settings line had **no `fov` field at all**,
     because the slider did not exist yet, so it ignored the operator's setting and rendered
     the authored ~43 degrees where the current build renders 67.64 — and it culled at 16:9
     on a 21:9 screen. **It was not faster; it was showing far less of the world**, and the
     part it was not showing was the exact defect the operator had asked to be fixed. With
     that separated, the thing actually under test — the whole RT era — cost **0.5-0.7 ms**.
     An old binary lacks *every* fix since, including the ones that silently change what the
     workload IS, and a settings field that is simply absent produces no warning anywhere.
     **Before quoting an old-binary delta, diff what the two builds PARSE and what they
     RENDER, not just what you changed.** The cheapest check is the settings/config line each
     one prints at startup. Whole-binary form of gotcha 423 and of
     `an-arm-that-removes-the-fix-too`.

440. **A VERIFIER READING ZERO OVER MILLIONS OF CHECKS CAN BE RIGHT AND STILL NOT COVER THE
     DEFECT — AND THE OPERATOR'S "I MIGHT JUST NOT HAVE SEEN IT" IS THE CORRECT OBJECTION.**
     Item C's gather verifier read **0 disagreements over 17,948,265 gathers** and was
     correct: it checks that the gather copied what the shader's list NAMES. The reported
     defect — a half-screen sky flicker — lives in what happens to the memo slot afterwards,
     and on a title that tiles LEFT/RIGHT a slot holding a partly-patched projection serves
     the two tiles different constants. **A verifier's scope is not the feature's blast
     radius.** Two process lessons ride along. First, when the only instrument is an eye,
     **run the positive control rather than banking the clean result** — the operator said
     *"not sure if I just didn't see it in the last run"*, which is exactly right for an
     intermittent defect, and the fix was two runs each way with the arm proving engagement
     from a line the feature prints. Second, **turning a feature off by default is a
     legitimate result**: the code, the arm and the offline gate all stay, and 0.8 ms of a
     16 ms frame against a visible defect in the shipped configuration is not a close call.

441. **A GATE WITH A WEAK MODE WILL BE RUN IN ITS WEAK MODE FOREVER.**
     `tools/alu_const_gate.py` checks the constant-gather's register lists against the
     shaders' own HLSL, and that cross-check is the ONLY thing that can catch a missing
     register — no run-time check can, because the missing register is by definition the one
     nobody reads (gotcha 432). It takes `--hlsl-dir`. **Every run this project ever made of
     it omitted that flag**, printing *"the lists were NOT cross-checked against the
     shaders"* into a log nobody read as a failure, and it was quoted as "clean on all 16
     caches" in three parts' gate sweeps. The reason was mechanical: `build_shader_spv.sh`
     generates the HLSL into a `mktemp -d` and deletes it on exit, so the strong mode needed
     a rebuild nobody would do casually. **When a check has an optional deep mode, make the
     inputs it needs survive by default, and make the weak mode SAY it is weak in the same
     line as its verdict** — this one did say so, and it still went unread for three parts,
     because a caveat printed beside an exit code of 0 reads as an exit code of 0. Sibling
     of gotcha 25 (a grep that cannot match is not a clean result) at gate scale.

442. **A LIST OF "WHAT THE SHADER READS" IS NOT A LIST OF WHAT THE RENDERER READS.** The
     constant gather copies only the registers each shader's sidecar names, and every gate,
     verifier and census built around it asked the same question: does the shader get what it
     reads? All of them passed, and the picture was still wrong — because **the renderer
     reads the constant window too**. `PatchFovProjection`/`PatchWideProjection` call
     `SceneXformForm`, which inspects c0..c3 to decide whether the window is a scene
     projection and then rewrites it for the fov slider and the 21:9 widening. Three shaders
     of 449 do not list all of c0..c3, so for their draws the patch was inspecting **arena
     residue**, never recognised a projection (0 of 379,968), and left those draws with an
     unpatched projection while the rest of the frame was widened. **When you make a copy
     conditional on a consumer's declared needs, enumerate EVERY consumer — including your
     own code downstream of the copy.** The fix was sixteen dwords; finding it took a day.
     Gotcha 440 one level down.

443. **A ROUTE TUNED FOR ONE DEFECT CAN BE ACTIVELY HOSTILE TO ANOTHER.**
     `tools/autoroute.sh` swings the camera left and right, because the operator's
     authorisation was *"move the camera to right or left for 30 second to try to reproduce
     stutter"*. Reused unchanged for a HALF-SCREEN SKY FLICKER hunt it made the defect harder
     to see, in their words *"it just makes the flicker harder to catch"* — a swinging camera
     changes which half of the sky is bright, which is the very signal the eye is watching
     for. It also polluted the automatic metric, whose first statistic counted exactly those
     camera-driven sign changes. `STILL=1` holds the view and the known-bad binary flickers
     on it reliably. **Before reusing a harness, ask what it was tuned to provoke and whether
     that provocation masks what you are now looking for.**

444. **RUN THE SAME ARM TWICE BEFORE QUOTING AN ORDERING OF FOUR ARMS.** Part 74 built a
     per-frame sky-asymmetry metric, ran four configurations once each, and reported the
     ordering as a result: 0.0708 flickering against 0.0130 clean. Re-running the SAME
     configuration later gave **0.0448**. With three runs an arm the medians do separate
     (0.024 vs 0.066, 2.7x) but the individual ranges **overlap**, so the metric is a
     three-run aggregate and was never capable of settling a single run. Two further
     lessons ride along: the first summary statistic chosen (sign flips) did not discriminate
     at all because the route's own camera motion produced them, so **collect the raw
     per-frame series and choose the statistic after looking at the data**; and when a
     defect is intermittent, the human verdict stays the ground truth until the instrument
     has been shown to separate the arms — not before. Sibling of gotchas 50/51/86 and of
     `every-campaign-needs-a-null-control-arm`.

445. **A CLOCK THAT STARTS AT THE OBVIOUS OPERATION MISSES THE EXPENSIVE ONE.** Part 74
     clocked the texture upload from the staging `memcpy` — the operation the function is
     named for — and reported 244 ms a run. Everything BEFORE that point was outside it: the
     allocation, the **untiling of every mip level**, the endian swap and the image creation.
     Those are **469 ms, 66% of the path and 209 us per texture against the measured 109**.
     For two parts the upload path was quoted at a third of its true cost, and the burst
     frames had ~230 ms unattributed *inside code the instrument was pointed at*. The tell
     was available and ignored: the frame's CPU time exceeded the sum of everything measured
     in it, which is the same arithmetic the residual column was built to enforce one level
     up. **When a measured phase does not account for the frame it sits in, extend the clock
     backwards to the first thing the work touches, not forwards from the API call.** Sibling
     of gotcha 435 (a counter named for the function it sits in measures the call).

446. **A `HOST_VISIBLE | HOST_COHERENT` MAPPING WITHOUT `HOST_CACHED` IS WRITE-COMBINED,
     AND READING IT BACK IS AN UNCACHED ROUND TRIP TO DRAM.** Part 75's re-baseline made
     the `constants` phase the single biggest term in a crowd frame — 44% of it — and
     splitting it twice put essentially all of that in one place: the fov/21:9 projection
     patch, **7.27 ms of a 19.99 ms frame at 5,000-7,000 draws**, against 0.36 ms for the
     constant GATHER sitting immediately next to it and 0.13 ms for the entire pixel
     window. What the patch does is read sixteen floats, decide whether they are a scene
     projection, and multiply two rows. It does that twice per draw (`PatchFovProjection`
     and `PatchWideProjection` each call `SceneXformForm`) on ~97% of draws — and it read
     them **out of the per-frame arena**, which this renderer allocates as
     `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`.
     On this machine that resolves to memory type 3; type 4 is the one that also carries
     `HOST_CACHED`. **Without `HOST_CACHED` the mapping is uncached — write-combining.**
     Writes are fine and that is what the flag combination is *for*; reads keep no cache
     line, get no prefetch, and cost a full memory round trip each.
     The fix is to never read the mapping: the sixteen floats are a verbatim copy of
     sixteen floats in the ordinary cached register file, so recognize and patch THERE and
     store the result back as one contiguous 64-byte write. Same bytes, verified 0 of
     47,352,900 draws; **-35% to -36% of the frame at 5,500-8,500 draws** in a pinned
     same-binary matched-band A/B.
     **AND THE SIZE OF IT IS THE POINT.** A control run at 16:9 — where this renderer's
     patch path is entirely inert, so nothing reads the mapping at all — profiles the same
     scope at **0.030 us per draw against 1.21**. The whole 7.6 ms was **ONE 64-byte read,
     once per draw**. Not a loop, not a copy, not a megabyte: sixty-four bytes. Write it
     down that way, because "we read a little mapped memory in a hot function" does not
     sound like a third of a frame, and is.
     **The general form, and it is a day-one check on any port that maps GPU memory:
     `grep` every mapped pointer for a READ.** A write into WC memory looks the same in
     the source as a read out of it, and only one of them is cheap. Related: gotcha 363 /
     the geometry-in-VRAM refutation, which is this same fact one buffer over — "geometry
     in VRAM is wrong for a renderer that reads its own geometry" — and part 73's 4.2x.
     The two findings share a mechanism and neither was found by reading code.
     **AND THIS PROJECT HAD ALREADY FOUND IT ONCE, AT A DIFFERENT POINTER.** Part 17
     discovered the PRESENT READBACK buffer was write-combined — 3.7 MB read back at
     ~230 MB/s, `readback` 15.7% -> 0.4%, the frame 103 -> 87 ms — and fixed it by adding
     `HOST_CACHED` there (`ReadbackMemoryProps`, `CZ_VK_READBACK_UNCACHED=1` the arm). The
     fix was applied at that one mapped pointer and never generalised, and the arena's own
     read-back then went unnoticed for **58 parts**. So the transferable instruction is
     not "watch out for uncached readbacks" — this project knew that. It is: **when you
     find a property of ONE mapping, enumerate every other mapping in the process the same
     day.** There were four here and the audit is one grep.

447. **AFTER YOU OPTIMISE A PHASE, SPLIT IT AGAIN — THE REMAINING COST MAY BE SOMETHING
     ELSE WEARING THE SAME NAME.** `constants` was named for "the per-draw ALU constant
     copy into mapped memory", and for four parts that is what everyone priced it as.
     Part 74 shipped the gather against it (item C, ~0.8 ms and 88.74 GB not copied) and
     the column stayed enormous, which read as "the copy is still expensive". It was not:
     the copy is 0.36 ms and 4.5% of its own phase. **A phase name survives the work done
     under it, and a phase that was 80% one thing before a fix is not 80% that thing
     after.** The tell was free and nobody looked: the phase had not shrunk in proportion
     to the bytes the gather removed. Sibling of gotcha 327 (splitting a phase has found
     every item; reading code has found none) and of "find the cost before optimising the
     name" — the addition is that the re-split is owed *by the fix itself*, in the same
     part, and costs one build.

448. **A DETERMINISTIC SCREEN EVERY RUN PASSES THROUGH IS A FREE MACHINE-STATE
     FINGERPRINT — USE IT TO REJECT COMPARISONS, NEVER TO NORMALISE THEM.** Part 75's
     six-run A/B was nearly reported at −37% off a line fit that was wrong three separate
     ways, and what exposed it was the raw per-window series rather than any summary.
     Every `autoroute.sh` run parks on the DebugJump menu for a few windows at the same
     draw count (~2,470-2,570, agreeing to 3% across every run this project has made of
     it). **Runs early in the session read ~9.0 ms there and later ones ~5.3 ms** — a 1.7x
     difference at an identical, deterministic screen, i.e. the machine, not the change.
     That makes the menu window a state tag stamped on every run for free, and the rule is
     **runs are only comparable to runs in the same state.**
     **But you may not divide by it.** The obvious next move — scale the slow-state runs by
     the menu ratio — is wrong here and provably so: scaling one arm's crowd window by
     5.3/9.0 disagrees with a directly measured same-arm run at the same draw count by 30%.
     The slowdown is not a uniform multiplier, so the fingerprint can only ever say
     "these two are not comparable", which is exactly the claim that saves you.
     Two corollaries this cost:
     * **Gate on a FINISHED log.** `autoroute.sh` exits 3 and prints "DID NOT REACH THE
       OUTDOOR WORLD — this log is NOT reportable", and a run still being written looks
       identical to one that failed. One run was called a gate failure on a partial read
       and it was not. Check the process is gone first (the memory note
       `a-partial-stats-read-measures-a-different-PLACE`, one level over).
     * **Never send an A/B loop's output to /dev/null.** The route gate fires there and
       nowhere else. One control run spent all 28 of its windows on the menu and went into
       the aggregate as if it were a crowd run, because the warning had been discarded.

449. **A SLOWER CONTROL ARM CAN FAIL A TIMED ROUTE MORE OFTEN THAN THE FAST ARM, WHICH
     BIASES WHICH CONTROL RUNS SURVIVE.** `autoroute.sh` walks the DebugJump menu with
     fixed-length press intervals and each press taps for 150 ms; part 54 had to fix that
     edge once already because a missed poll window costs the whole outdoor route. A
     configuration that renders more slowly polls fewer times inside the same window, so it
     is *more likely* to miss — and the runs that survive the gate are then the arm's
     luckiest ones. Part 75 saw the control arm miss the route where the fix arm did not.
     **The fix is to make the route's timing generous enough that neither arm can miss it
     (`PRESSMS`), and to report the per-arm GATE FAILURE RATE beside the result** — a
     silent difference in how often an arm completes the route is a selection effect
     wearing the shape of a measurement.

450. **AN OPTIMISATION CAN BE SILENTLY CANCELLED BY THE LAUNCHER EVERYONE ACTUALLY USES.**
     Part 54 moved this port to a Vulkan swapchain and the whole point was that "the
     present readback and its two copies do not run". Part 75's operator session measured
     `readback` at **3.49 ms of a 23.31 ms crowd frame** — under the swapchain. Both are
     true: the readback survives the swapchain exactly when a PICTURE INSTRUMENT is armed,
     because every one of them walks the presented frame on the CPU, and the predicate is a
     `static` read once at startup. **`tools/play_session.sh` — the operator's standard
     launcher — sets `CZ_CAPTURE_KEY` and `CZ_BURST_DUMP` unconditionally, so F8 and F9
     work.** So every play session since part 54 has paid a 19.8 MB `memcpy` per frame into
     a buffer the swapchain never displays, to make a key work that gets pressed five times
     in an hour.
     The runtime even SAYS so, in its own log, on the line above the one anybody reads:
     *"swapchain present with a picture instrument armed: the present READBACK IS STILL
     RUNNING... its `readback` column is NOT this arm's cost."* It was written as a caveat
     for measurement and it is really a description of the shipping configuration.
     **Three transferable parts.** (a) When you make something conditional, go and read
     every launcher and script in the repo for whether it sets the condition — the flag's
     DEFAULT is not the configuration users run. (b) An edge-triggered feature (a
     screenshot key) must not force a per-frame cost; arm it on the press and take the
     frame after, because one frame of lag on a still is nothing. (c) A caveat aimed at
     "do not quote this number" can be hiding "do not SHIP this configuration" — the same
     shape as gotcha 441, where a caveat printed beside exit 0 read as a passing gate.

451. **MAKING A STATIC PREDICATE DYNAMIC BREAKS AN IDENTITY NOBODY WROTE DOWN — CHECK
     EVERY PLACE THE OLD ANSWER WAS REUSED ACROSS A PIPELINE BOUNDARY.** Part 76 turned
     "does the present readback run this frame" from a `static const bool` read once at
     startup into a per-frame decision armed by a key press. The predicate itself was
     three lines and correct. What was NOT correct was the code downstream, which had been
     written against the old invariant without knowing it: the readback is *recorded* for
     the frame being submitted, and the pixels are *read* out of the frame being retired,
     one to two frames older. While the answer never changed, `doReadback` was a true
     statement about both frames and the same variable served both roles. The moment it
     could change, testing the recording frame's decision while dereferencing the retiring
     frame's buffer meant the first frame after a press handed the burst recorder a slot
     whose last real contents were nine seconds and a camera sweep old — **with the
     correct frame number written beside it in the manifest**, so the artifact looked
     entirely well-formed. The fix is one bool on the frame slot (`hasPixels`), recorded
     with the pixels and read at the retire.
     **The transferable part is the search, not the fix.** When you make a constant into a
     variable, the defect is never at the site you changed — it is at every site that
     silently depended on the constancy, and those sites do not mention the flag. Grep for
     the *consumers of what the flag guards*, not for the flag. Here that was one `px` and
     five instruments reading it. And note what caught it: not a test, but asking "which
     FRAME is this decision about?" while writing the comment for the change — which is an
     argument for writing the comment before the commit rather than after.
     **The gate that would have caught it needed a canary, not an assertion.** A stale
     buffer produces a plausible picture with a correct label, so nothing structural can
     see it; what separates them is CONTENT, and the cheapest content oracle to hand was
     the OTHER artifact of the same run — no burst frame may be byte-identical to the F9
     capture taken nine seconds earlier, which is exactly what a stale slot would serve.
     It is a canary and not a proof (two genuinely identical frames would also match), and
     it works here only because the route keeps the camera turning. Say which of the two
     you have built.

452. **A NEAR-NULL CONTROL CHANNEL IS ONLY A CONTROL FOR CHANGES THAT CANNOT REACH IT —
     AND WHEN ONE DOES, YOUR TOOL WILL REFUSE THE COMPARISON RATHER THAN REPORT IT.**
     `tools/part75_ab_report.py` groups A/B runs by a MENU-window fingerprint: the
     DebugJump screen is identical every run, so a difference there is the machine drifting
     and not the change, and runs are compared only within one state. That is sound, it
     caught a real 1.7x machine drift in part 75, and in part 76 it printed
     `NOT COMPARABLE, no matched control` for **both arms of the cleanest A/B this project
     has run** — six runs, monotone across every band, an effect ten times the null floor.
     The reason is that part 76's item touches EVERY PRESENTED FRAME, menus included, so
     the menu window moved −15.2%: exactly the same as the crowd. The fingerprint was
     measuring the change and was read as measuring the machine.
     **The rule is to classify the CHANGE before choosing the channel.** A control channel
     assumes the change cannot reach it, and that assumption is a claim about the fix, not
     about the channel. For a crowd-only item the menu is a state check; for a per-frame
     item it is a SECOND MEASUREMENT and a rather good one. `tools/part76_band.py` prints
     it as a number and says which of the two it is, instead of partitioning on it.
     **And notice the failure mode**: it did not produce a wrong number, it produced a
     REFUSAL — which is the safe direction, and is also the direction that gets a real
     result thrown away by a session in a hurry. A tool that declines should say what
     assumption it declined on (see gotcha 441: a caveat beside exit 0 reads as exit 0;
     this is the same defect with the sign flipped).

453. **A CPU SAVING BELOW THE GPU FLOOR MEASURES ZERO, AND EVERY INGREDIENT OF YOUR
     PREDICTION CAN BE RIGHT WHILE THE PREDICTION IS WRONG.** Part 76 found two `getenv`
     calls on the per-draw path, priced the OPERATION by microbenchmark (60.6 ns for a miss,
     67.5 for a hit, in a 100-121 entry environment), confirmed the CALL COUNT with two
     temporary counters (62,842,293 hits against 60,176,862 draws — 1.04 per draw, exactly
     as assumed), multiplied, and predicted **~0.77 ms a frame**. A six-run A/B measured
     **0.08 ms against a 0.06 ms null** — inside the floor. Both ingredients were right. The
     missing third was whether the frame was still WAITING on that thread: an hour earlier,
     the same part had taken 4.15 ms of CPU off the same frame, and `CZ_VK_FRAME_TRACE` now
     read **GPU 10.55 ms of a 10.59 ms wall — 100% — with a 2.99 ms fence wait**. The CPU
     was finishing early and idling.
     **Three transferable parts.** (a) A per-frame CPU prediction is `ns x calls x
     (is the CPU the limiter?)`, and the third factor is the one nobody writes down; get it
     from a fence/GPU column, not from an assumption. (b) **Your own last fix is the most
     likely thing to have moved that factor** — this project spent 76 parts CPU-bound, part
     74 concluded "the GPU is never the limiter here", and part 76's own item 1 crossed the
     floor in the same session; re-read the floor AFTER shipping, not before. (c) A change
     that is semantics-identical and cannot be slower still SHIPS — but as a correction, not
     as a saving, and the honest report says the arm measured zero and why.

454. **AN INSTRUMENT WHOSE BILL LANDS ON THE RESOURCE YOU ARE MEASURING CAN INVERT THE
     VERDICT, NOT JUST INFLATE THE NUMBER.** `CZ_VK_PROFILE` has been documented at "2-4 ms
     a frame" for twenty parts, which reads as a tax to subtract. Measured properly for the
     first time in part 76 — same route, same band, same binary, the frame trace armed in
     both arms — it costs **+4.00 ms of CPU record and only +1.01 ms of wall**, because the
     frame had three milliseconds of CPU slack and the instrument ate them. And the fence
     wait went **2.99 ms -> 0.00**: *the same route reported as GPU-bound without the
     profiler and CPU-bound with it.*
     **The general shape:** a "which resource is the limiter" question is answered by
     SPARE CAPACITY, and an instrument that consumes spare capacity on one side destroys
     the very quantity being asked about. A frame-time instrument taxes a frame-time answer
     linearly and you can subtract it; a CPU instrument taxes a CPU-versus-GPU answer
     NON-linearly and you cannot, because the wall time absorbs the tax until the slack runs
     out and then stops absorbing it.
     **So:** for a regime question, run the CHEAPEST instrument that can answer it —
     here `CZ_VK_FRAME_TRACE` alone, one line of I/O per frame, which does not move the
     numbers — and reach for the phase profiler only once you know which side of the frame
     you are attributing. Quote a regime verdict with the instrument load beside it the way
     this project quotes a frame time with its resolution (gotcha 447), and if you must
     subtract, say that the bill probably scales with the load and in which direction that
     biases the conclusion. Related: gotcha 7 (a probe expensive enough to stall the game
     manufactures the stability it reports) and gotcha 223 (an instrument on a hot path can
     cancel the effect it is measuring exactly).

455. **A SCOPE'S NAME IS A LIST OF WHAT SOMEBODY EXPECTED TO FIND IN IT, AND THE THING THAT
     COSTS THE MOST IS OFTEN INSIDE ONE OF THE WORDS.** This project's texture `decode`
     clock is named *"untile + endian swap + image creation"*. Split in part 77, it holds
     **seven** distinct operations, and the largest is `vkAllocateMemory` — **70.7% of the
     scope, 145 us x 2,424 calls** — hiding inside the third word. The untile loop, which
     three consecutive hand-offs named as the fix and which had been priced, cross-checked
     and scheduled, is **17.2%**.
     Part 75 hit the same shape (`constants` was 43% of the frame and the copy everyone
     would have looked at first was 0.36 ms of 7.63) and part 74 hit it twice. **The
     recurring repair is the same and it is cheap: before optimising the operation a phase
     is named for, SPLIT THE PHASE, and print the residual so a wrong split shows up as an
     unattributed column instead of as a confident wrong ranking.** In part 77 the split was
     forty minutes of work — eight clock reads per upload, ~2,300 a run, nowhere near a hot
     path — and it moved the item from an 85 ms target to a 351 ms one before a line of the
     fix was written.

456. **AN ITEM CAN BE SPECIFIED, PRICED AND CONFIRMED BY THREE SESSIONS OF OPERATOR EVIDENCE
     AND STILL BE WRONG, BECAUSE ALL OF THAT CONFIRMED ITS PARENT.** Part 77's item 1 was
     the most thoroughly evidenced thing this port has ever carried: 36 F7 marks across
     parts 74, 75 and 76 all landing on texture frames, a two-half cost breakdown, a named
     fix per half, a pre-registered kill threshold, and three kickoffs repeating it verbatim.
     Every one of those confirmations was of the claim **"the texture path is the hitch"**,
     which is true. None of them touched the claim **"the untile loop is where the decode's
     time is"**, which was never measured and is false.
     **Confirmation of a parent does not transfer to a child.** When an item has a chain —
     *the frame is slow -> the texture path -> the decode half -> the untile loop* — the
     evidence usually attaches at the top and the FIX attaches at the bottom, and the links
     in between get carried forward on repetition rather than on measurement. Ask, of the
     specific line you are about to change: what measurement says the time is *here*, and
     when was it taken? See also gotcha 13 (everything written down has a shelf life) —
     but this is worse than staleness, because the child claim was never true, only
     never checked.

457. **A POOL BUILT FOR A NEW SUBSYSTEM THAT CITES THE OLD ONE AS PRESSURE TO BUDGET AROUND
     IS TELLING YOU THE OLD ONE HAS THE SAME DEFECT.** `vk_renderer.cpp` grew an
     acceleration-structure pool whose comment reads: *"a crowd's ~2,600 BLASes against the
     driver's ~4096 `maxMemoryAllocationCount` would exhaust the allocator **with textures
     still to serve**."* The hazard was named exactly right, the pool was built correctly —
     and the texture path, the thing being budgeted around, kept one dedicated
     `VkDeviceMemory` per image for another twelve parts, at 145 us each and 2,424 a run.
     The author reasoned about the neighbour as a fixed constraint instead of as the thing
     to fix.
     This is gotcha 446 in a new place (part 17 fixed a write-combined read at the present
     buffer and part 75 found the same defect at the constant arena 58 parts later), and the
     repair is the same: **when you fix a resource-usage defect, grep for every other place
     that uses the resource the same way, in the same commit, and record the audit as the
     deliverable.** The tell to look for in a codebase is a comment that names another
     subsystem's consumption as a reason for your design — it is a measurement somebody
     already made of a cost they then declined to touch.

458. **THE VULKAN VALIDATION LAYER IS BLIND TO IMAGE LAYOUTS IN A BINDLESS,
     UPDATE-AFTER-BIND DESCRIPTOR ARRAY — AND IT REPORTS CLEAN, NOT UNKNOWN.** Part 77 built a
     deliberately broken build (`CZ_VK_TEX_BATCH_BREAK=1`) that skipped the flush ordering
     texture copies ahead of the frame that samples them. It left **~1,400 textures never
     copied to the GPU at all**; thousands of draws sampled images in `UNDEFINED` layout with
     no contents. `CZ_VK_VALIDATION=1` reported **only the six pre-existing pipeline VUIDs**,
     the route gate passed, and every draw counter read healthy. The picture was FULLY BLACK.
     The layer cannot statically associate a descriptor in a large update-after-bind array
     with a draw, so it does not track those images' layouts — structural, not a layer bug,
     and it says nothing rather than saying it cannot tell.
     **The lesson is not "validation is unreliable"** — it named the white-surface UB in one
     run and is still the first thing to reach for on pipelines, barriers on named images and
     view/type mismatches. It is that **every gate has a blind region, and the only way to
     find yours is to build the broken version and check the gate screams** (gotcha 30,
     and gotcha 444's "a gate with a weak mode will be run in its weak mode forever"). Here
     the working build and the black-screen build were *indistinguishable* to the gate that
     had been chosen for them.
     **What DID have power** was the era-median picture comparison with a null pair
     (`tools/frame_era_medians.py`): coverage 99.87% -> 0.0000%, **43,421x the null.** For any
     change to WHEN a resource is written or transitioned, gate on the picture or on an
     explicit invariant counter — something whose reading is produced BY the change — not on
     a checker that may not be looking.

459. **A PARTITION THAT IS EXACT BY CONSTRUCTION HAS NO WAY TO SAY IT IS WRONG — SPLIT A
     TOTAL WITH EXPLICIT INTERVALS, NOT WITH A CHAIN OF BOUNDARIES.** Part 78 needed the
     frame's GPU time split by region. The obvious design is a chain of timestamps at every
     boundary: consecutive deltas partition the frame exactly, every nanosecond is
     accounted for, the numbers always add up. **That last property is the defect.** A
     chain cannot have a residual, so it cannot report that a region is missing, mislabelled
     or wrapped around the wrong work — it will confidently charge unwrapped work to
     whichever class happens to come next. The instrument shipped as explicit (begin, end)
     PAIRS around named regions, with everything between them unattributed and printed
     FIRST, and that residual moved 16.7% -> 3.5% when the barriers were brought inside the
     regions — which is how the barriers became the finding rather than staying invisible.
     Same shape as part 77's decode residual (gotcha 456) one level down: **read the
     residual first; a large one means the split is wrong, not that work vanished.**

460. **A CLASSIFIER THAT READS SOMEBODY ELSE'S COUNTER READS IT AFTER THEY RESET IT.**
     Part 78's per-pass GPU split bucketed each render scope by `R->drawsThisPass` at the
     point the scope closed, and reported **65% of the frame's GPU time in passes with ZERO
     draws** — a bucket that cannot exist. `DoResolve` zeroes that counter in its histogram
     block *before* the `EndRendering` that closes the scope. The §4b cycle clock two hundred
     lines above says so in a comment and captures its own class at entry for exactly this
     reason; it was read and the bug was written anyway. The fix is not to capture earlier,
     it is to **own the counter**: one the instrument increments and resets itself cannot be
     reset out from under its reader by code that knows nothing about it. The tell was that
     the impossible bucket was also the LARGEST one — a classifier whose dominant class is
     one you cannot explain is misreading its key, not discovering something.

461. **THE ALWAYS-CORRECT SYNCHRONISATION PRIMITIVE IS THE ONE NOBODY EVER MEASURES.**
     Every image barrier in this renderer used `ALL_COMMANDS -> ALL_COMMANDS` with
     `MEMORY_READ | MEMORY_WRITE`. That is the right thing to write while a renderer is being
     built — it cannot be too weak, so it can never be the cause of a corruption, and it
     therefore never appears in any bug hunt. **It was 0.930 ms of an 8.49 ms GPU frame,
     11.0%**, because this renderer oscillates two full-size EDRAM images between attachment
     and transfer layouts 49 times a frame and each barrier was a full pipeline drain plus a
     cache flush. Deriving the masks from the layouts took it to 0.126 ms.
     **The general form: a construct chosen because it is conservatively safe is invisible to
     correctness work by definition, so the only thing that will ever price it is a
     measurement you go and take.** Ask of any such construct how many times a frame it runs
     before assuming its width is free.

462. **CORRECTING ONE THING CAN EXPOSE A HAZARD A NEIGHBOUR WAS HIDING — AND THAT HAZARD WAS
     ALWAYS THERE.** With the barrier masks narrowed, synchronization validation reported ten
     `WRITE-AFTER-READ` on the SWAPCHAIN image per run: its `PRESENT_SRC -> TRANSFER_DST`
     transition is scheduled at `TOP_OF_PIPE` while the acquire semaphore is waited at
     `TRANSFER`, so it can write an image the presentation engine has not released. That
     barrier is hand-written and part 78 did not touch it. It was silent before **because
     every other barrier in the frame used ALL_COMMANDS and formed a dependency chain the
     layer accepted** — the defect was riding on its neighbours' over-synchronisation.
     Do not read "the new arm reports hazards the control does not" as "the new arm is
     wrong"; ask which of the two is *hiding* something. And when a fix removes accidental
     protection, the places that were relying on it are a list you must go and find, not a
     regression.

463. **AN EARLY RETURN IN A BARRIER HELPER IS A MISSING DEPENDENCY, NOT A SAVING.**
     `Barrier(cmd, img, layout)` returned immediately when the image was already in the
     layout asked for. That is correct for a *transition* and wrong for the caller's actual
     question, which is "may I write this now": two consecutive `vkCmdClearDepthStencilImage`
     calls on the same surface got **no ordering between them at all**, and which clear value
     survives is undefined. This title issues ~41 colour and ~41 depth clears a frame, so
     consecutive ones happen. Synchronization validation named it in both the new and the old
     arm, i.e. it had been there since the renderer was written and no picture instrument had
     ever been able to see it. **A helper named for the mechanism (`Barrier`) rather than for
     the guarantee (`safe to write`) will be called at sites needing the guarantee, and its
     fast path will silently not provide it.** Have it report what it did — this one now
     returns whether it transitioned, and the write sites issue their own dependency when it
     did not.

464. **A FIX ON ONE SIDE OF A BOTTLENECK RE-PRICES EVERY ITEM ON THE OTHER SIDE — RE-DERIVE
     THE REGIME, DO NOT CARRY THE TABLE FORWARD.** Part 78 took 11.5-14.3% off the GPU at the
     operator's load. Almost none of it reached their frame, because they were CPU-bound
     there — which is exactly what was predicted and looks like a disappointing result. **It
     is the opposite.** The GPU headroom at their crowd roughly doubled (0.58 -> 1.93 ms at
     7,000-9,000 draws, 1.38 -> 2.38 at 9,000-12,000), every band from 3,000 draws up moved
     from GPU-bound or balanced to CPU-BOUND, and the fence wait went to 0.00 everywhere
     above 3,000. **The CPU items on the backlog are now worth roughly three times what they
     were worth the day before, and nothing about them changed.**
     Two habits follow. **(1) A regime table has a shelf life exactly as a number does**
     (gotcha 13): `part77-kickoff.md` §0b's "crossover at 6,000-7,000 draws" was correct when
     written and wrong two parts later, and a session that carried it forward would have
     ranked its board on a dead fact. **(2) Judge a fix on the resource it targets, not only
     on the wall clock.** Reading part 78 as "−2.3% at the operator's crowd" is true and
     nearly useless; reading it as "−14.3% of the GPU, which converted the frame to
     CPU-bound and freed 1.35 ms of headroom" is what tells the next session what to do.
     The same shape as gotcha 453 read from the other end.

465. **A TIGHT NULL PAIR IS ONE SAMPLE OF THE FLOOR, NOT EVIDENCE THAT THE FLOOR IS TIGHT —
     AND ON A MOVING-CAMERA ROUTE THE FLOOR IS COMPOSITION, NOT PIXELS.** Part 79's picture
     gate ran two shipping runs as the null and they agreed to **0.05% on `meanLuma`**. The
     control arm then differed by 1.6% — **33.7x the null** — on a change that cannot alter
     a pixel by construction. It was the ROUTE: `frame_era_medians.py` aggregates every frame
     above 1,800 draws, the two arms' era median DRAW COUNTS were 4,573-4,851 against
     3,866-3,915, and the run is a steep luma ramp (era thirds 28.5 -> 68.5 -> 78.7), so
     where a run's frames land in that ramp moves the median far more than a renderer change
     can. Banded by draw count the same data agrees to ≤0.3% wherever the populations are
     comparable.
     **Two habits.** (1) `frame_era_medians.py` already warns that one null pair is one
     sample, for distinct colours; it applies to `meanLuma` just as hard, and a pair that
     happens to agree to 0.05% has told you nothing about a floor that is really ~1.5%.
     (2) **The remedy is to remove the composition variance, not to average more of it**:
     `STILL=1` holds the camera and the same comparison then reads 0.03% and 0.12% against a
     0.50% null — INSIDE, on every statistic — while the positive control still reads
     36,799x. Matched draw bands work too and are more work. See gotchas 254 and 249; this
     is the third distinct way this route's picture aggregate has misled a session.

466. **REMOVING A WAIT ON A GPU-BOUND ROUTE RELOCATES IT — MEASURE WHERE IT WENT BEFORE
     QUOTING THE COLUMN THAT FELL.** Part 79 removed the `vkQueueWaitIdle` inside
     `FlushTextureUploads`. The quantity it targeted collapsed exactly as designed — the
     flush went 999-1138 us to 106-114 us, −89.8%, with the three fix runs agreeing to 0.7 ms
     on the run total against the control's 5.1 ms spread. **The run got no faster: 19,337
     frames against 19,291 in the same 148.6 s, and the median FENCE wait on the affected
     population went UP, 0.699 -> 1.136 ms.** The pump stopped blocking in the flush and
     started blocking on the frame fence instead, because that route is GPU-bound at 6,200
     draws and it was going to wait for the device either way.
     This is gotcha 238 one level up (a column that falls to zero is not a saving until you
     find where the replacement work was charged) crossed with 453 (a CPU item measured in a
     GPU-bound regime reads zero whatever it is worth). **Both were known before the runs** —
     `part79-kickoff.md` §2 rules out CPU A/Bs on this route below ~8,000 draws in as many
     words — which is the actual lesson: when the hand-off says your route cannot price the
     item, believe it, build the fix, measure the MECHANISM, and say plainly that the number
     the operator needs does not exist yet rather than reporting the route's null as the
     item's value.

467. **A RARE ONE-FRAME COST IS INVISIBLE TO EVERY AGGREGATE AND IS THE ONLY THING THE PLAYER
     FEELS — AND ITS OWN COMMENT WILL TELL YOU IT IS FINE.** `PersistMaintenance` grows the
     cross-frame stream store, and its comment said *"it runs at most a handful of times a
     run"*, which is true and which is exactly why nobody priced it. A growth is
     `WaitAllFramesIdle` + `vkDeviceWaitIdle` + a host-visible allocation and MAP + freeing
     the old buffer, all on the pump inside ONE frame: **71.7 ms for a 256 MB step.** The
     operator's session grew twice and **both growths were the worst frame of their own
     ten-second window, and both were the only things they felt in 96.8 seconds of play.**
     A run mean cannot see two frames in 12,610; a p99 cannot; a banded median cannot.
     **The tell that identified it was the SHAPE, not the size**: an isolated single frame
     whose draw count, GPU time, uploads and pipeline count are identical to its neighbours,
     with fence 0.00 and sleep 0.00. That is never a workload — it is an allocation, a
     rehash, a growth or a free. Look for those before theorising about the frame's contents.
     And **put a clock on every "runs rarely" path**, because rarely-but-huge is the exact
     profile of a felt stutter.

468. **SPLIT THE COST BEFORE CHOOSING THE FIX, EVEN WHEN THE FIX LOOKS OBVIOUS.** The 71.7 ms
     growth above contains two `vkDeviceWaitIdle`-class waits, and "fence the old buffer away
     instead of idling" is the obvious, principled, more-interesting repair. Split three ways
     it is **waits 13.6 + allocate/map 42.9 + free-old 15.2** — the waits are the SMALLEST
     term, and that fix buys 19%. Only not growing at all removes the other 81%, and that is a
     one-line default change (128 -> 512 MB start), measured at **90.9/90.3 ms -> 36.7/33.6 ms**
     on the growth window's worst frame for **+10 ms on a boot frame already at 235**.
     Gotcha 238's shape a third time: the interesting half of a cost is not usually the big
     half. Two extra clock reads decided it.

469. **RAISING A DEFAULT IS HOW YOU BREAK A MACHINE THAT IS NOT YOURS — GIVE IT A FALLBACK,
     NOT AN ABORT.** The stream store's default start went 128 -> 512 MB. Its allocation
     failure path set `persistOn = false` and ran with NO store, which is a ~30% frame-time
     regression, and on a 48 GB development box that path is unreachable and therefore
     untested. It now retries at 128 before disabling. **When you raise a resource default,
     find the code that runs when the resource is refused and ask what it does on a machine
     smaller than yours** — "degrades gracefully" written for a 128 MB request is not
     necessarily a graceful degradation for a 512 MB one.

470. **WHEN A COST SCALES WITH A DOUBLING BUFFER'S NEW SIZE, RAISING THE START DOES NOT REMOVE
     THE CLASS — IT SKIPS THE CHEAP EVENTS AND LEAVES THE EXPENSIVE ONE.** Part 79 measured
     the stream store's growth at 71.7 ms for a 256 MB step and raised the start 128 -> 512 to
     remove the operator's two felt hitches (87 and 158 ms). It removed them, and their next
     session grew ONCE more, 512 -> 1024, for **329.2 ms in a single frame** — which they felt
     and correctly located ("a single stutter near the end... didn't feel one after loading").
     Two hitches at 87 and 158 became one at 352. **The arithmetic said so before the run**
     and I did not do it: if a step of size N costs f(N) and the container doubles, the LAST
     growth is always the most expensive one you will ever pay.
     The fix that works is structural: start at the ceiling, so growth is impossible by
     construction rather than unlikely. Ask "what makes this event impossible" before "what
     makes this event rarer".

471. **THE SAME ALLOCATION COSTS 25x MORE MID-RUN THAN AT STARTUP — MEASURE BOTH BEFORE
     DECIDING WHERE TO PAY IT.** A 512 MB host-visible allocation added **~10 ms to a boot
     frame already at 235**; a 1024 MB one mid-run cost **255 ms**, because mid-run it must
     find the pages while the old buffer is still live and the machine is under load. That
     asymmetry is what makes "reserve the worst case at startup" a free fix rather than a
     trade — the 1 GB arm's boot frame (226.9 / 227.9 ms) is the LOWEST of seven runs spanning
     three different starts, i.e. the allocation is inside the noise of a boot frame that is
     230-250 ms whatever you do. **A startup cost and a steady-state cost are different
     measurements and the ratio is not 1:1.**

472. **NAME THE REFUTATIONS IN THE HARNESS, AND ONE OF THEM WILL FIRE.**
     `tools/part80_operator_session.sh` listed three things that would refute part 79's
     attribution, the first being "a growth line appearing anyway — 512 was not enough for
     this session". That is precisely what happened, and because it was written down before
     the run the result read as a measurement rather than as a surprise: the operator's
     one-sentence report mapped onto a pre-registered branch, and the next fix followed from
     it in minutes. A harness that only describes success turns a refutation into a
     debugging session.

473. **AN ITEM'S PRICE HAS A SHELF LIFE EXACTLY LIKE A MEASUREMENT DOES — AND A SHARE IS NOT
     A PRICE.** Parallel command recording was the largest item on this project's board for
     five parts and was re-quoted UPWARD through three hand-offs, always as a share:
     *"`DoDraw` plus the driver, ~40% of the pump"*. Nobody re-measured the quantity
     underneath it. When one destructive arm finally asked — skip every `vkCmd*` in the
     record path and nothing else — the answer was **251 ns a draw**, because part 18's bind
     cache had meanwhile grown to elide **descriptor-sets 100%, blend 100%, viewport 99.4%,
     scissor 99.3%, pipeline 70%** of exactly the calls a parallel recorder would have
     distributed. The item was sized before the cache was that good and the size was carried
     forward as if it were a constant. **Before building the biggest thing on your board,
     re-measure the number that made it the biggest** — especially if the parts since then
     shipped anything in the same path. Two runs.

474. **A CHANGE DETECTOR CANNOT BE MEMOISED ON THE THINGS IT IS WATCHING.** Three separate
     "remember the answer" items died on this in one session: a vertex-fetch decode memo on a
     coarse key, the same memo on an exact key, and a per-draw stream-lookup dedup. Two of
     them had acceptable hit rates (41.2% and 53.8%) and failed anyway, because the work they
     proposed to skip was the call that runs the content GUARD — so a hit would serve last
     frame's geometry to a mesh the guest had edited in place, which is a defect this port
     already shipped once and had to hunt (the HUD serving stale ammo, gotchas 242-243).
     **Before designing a memo, ask what the work you are skipping was DRIVING**, not just
     what it was computing. If the answer is "a guard", the memo key would have to include
     the guard's own answer, and then there is no memo.

475. **A MATURE RENDERER'S REMAINING CPU COST IS ITS GUARDS, AND THAT IS A CEILING, NOT A
     BACKLOG.** After five parts of guard work (18, 22, 24, 47, 55), the per-draw CPU
     decomposition here has no term above ~0.6 ms that is both safe and threadless: the
     single largest mechanism in the frame is the Vulkan driver itself at 251 ns a draw.
     Every large win in this renderer's history came from REPLACING recomputation with a
     guard, and once that is done the frame is guards. Say so on the board explicitly —
     otherwise the next session spends itself proving the same thing item by item. **"There
     is no large item left here" is a finding and should be published as one.**

476. **WHEN THE FENCE IS ZERO, EVERY GPU ITEM ON YOUR BOARD IS WORTH NOTHING — AND YOU CAN
     KILL THEM WITHOUT A RUN.** Part 80's board carried two GPU items with measured ceilings
     (resolve clears 0.568 ms, resolve copies 0.699 ms). The regime table says fence **0.00
     at every band from 5,000 draws up** with 2.3-3.1 ms of headroom between the wall and the
     GPU — so the device already finishes early and removing GPU work moves nothing until the
     CPU has fallen by the whole headroom. Their combined ceiling is less than the headroom,
     so they cannot become live on their own. **Read the regime BEFORE ranking a board, not
     before measuring an item**: it does not merely tell you how to measure, it tells you
     which items are measurable at all. The converse bit too (gotcha 453, and part 79's
     six-run campaign on a GPU-bound route).

477. **A TRACE RECORDS CHANGES, SO AN ENTRY'S EXTENT IS A PROPERTY OF ITS SUCCESSOR.**
     Transcribing an operator's recorded input into a replayable recipe produced three
     separate wrong answers, all the same shape. (a) Ending each state at its own last sample
     invents a one-poll gap between every pair of inputs — twenty spurious stick releases
     during one continuous walk. (b) Resolving extents AFTER merging instead of before turns
     a button release, which is usually a single sample and therefore zero-length, into a
     glitch that gets absorbed into the press before it: a 145 ms menu press became a
     **2,248 ms hold**, which is not a slightly-wrong press but a different input. (c) An
     entry shorter than the consumer's poll interval is an input that never happens — a 1 ms
     recorded press has a 1 ms delivery window against a ~10 ms poll, so it lands about one
     time in twenty and the recipe silently stops one screen short. Floor it to the
     consumer's own tap length and **borrow the time from the following silence** so nothing
     downstream shifts. Applies to any event log driving a replay, not just input.

478. **A CARDINAL VOCABULARY CANNOT REPRODUCE A HUMAN, AND THE FAILURE IS SILENT.** A synthetic
     input arm with eight stick directions at full deflection replayed an operator's route and
     walked into a building; their words were *"the character goes forward the whole time
     while I was often slightly to the left"*. The trace agreed exactly — over a 14.5-second
     walk the Y axis was pinned at 32767 while X drifted **-5,467..+3,993**, a 17% deflection
     that is steering. Rounding that to the nearest of eight does not approximate the route,
     it produces a different route that starts in the same place — and the run still arrives
     somewhere and still reports a draw count, so nothing fires. The fix is analog entries
     (`LS<x>/<y>`) and the ability to hold BOTH sticks at once, because a player turns the
     camera while walking and the camera decides the draw set. **The person who played it is
     the only oracle for whether a replay is the same journey.**

479. **AN IMPLICIT VULKAN LAYER CAN BE IN YOUR DEVICE DISPATCH CHAIN FOR MONTHS WITHOUT
     ANYONE ASKING — CHECK, AND THEN MEASURE RATHER THAN ASSUME IT COSTS.**
     `VK_LOADER_DEBUG=layer` on one 40-second run found **`VK_LAYER_LS_frame_generation`**
     inserted as an instance *and device* layer in this process. Its manifest has a
     `disable_environment` and **no `enable_environment`**, so it is on by default, and the
     package predated the whole port by three months — it had been in the chain of every run
     ever made here, mine and the operator's. A device layer is in the per-draw call path by
     construction, so this looked like an unmeasured tax on 45,000 `vkCmd*` calls a frame.
     **It was a null**: `record` read 637/645 ns/draw with it and 639/639 with it disabled,
     where the two default runs differed from each other by more than the arms did.
     `nm -D --defined-only` said why in advance — the library **defines no `vkCmd*` entry
     points at all**, so the loader's dispatch table holds the driver's own pointers. Two
     lessons, and the second is the one that generalises: **enumerate what is in your chain
     before quoting any per-call number as "the driver"**, and then **let `nm` predict and an
     A/B decide**, because "a layer is present" and "a layer costs" are different claims.

480. **A CALL COUNT IS A SHAPE, NOT A VOLUME — LOOK FOR THE API THAT TAKES AN ARRAY.**
     The largest single driver cost in this renderer turned out to be
     `vkCmdBindVertexBuffers` at **1.725 calls a draw, 36.8% of all driver calls, 0.83 ms a
     frame** — not because it is slow or because there are many streams, but because the
     helper issues **one call per binding** while the API takes a **contiguous range**. The
     bind loop was already assigning bindings with `++binding`, so they were contiguous by
     construction and the batch was free for the taking. **When a per-draw cost is dominated
     by one entry point, count the CALLS before optimising the WORK** — and check whether the
     API has a plural form you are not using. The same question is worth asking of every
     `…(…, 1, &thing)` in a hot path.
     **And the saving is a distribution question, not an average one:** 1.725 changed
     bindings of 3.310 offered is consistent with "52% of draws change all their bindings"
     (batching saves 0.58 ms) and with "changed bindings interleave with unchanged ones"
     (batching saves nothing). Those differ by the whole item, and only a run-length census
     tells them apart — a mean cannot.

481. **TWO COUNTERS PRINTED ON DIFFERENT LINES ARE NOT A MATCHED PAIR — CHECK BEFORE YOU
     SUBTRACT.** `guard read 86.21 MB/frame` minus `27.2 MB/frame served by the prehash pool`
     produced "59 MB a frame is still read on the pump", which at any plausible rate is
     milliseconds. It survived into a plan as *"the largest number in the frame that has never
     been placed"*, and a whole item was built on it. Measured directly, the pump reads
     **1.11 MB/frame over 67 hashes, 0.077 ms a frame** — five times below the route's noise
     floor — and the pool serves **97.1% of the BYTES**. The two counters were reset over
     different windows with different denominators, so the subtraction was between numbers
     that were never a pair. **When a subtraction produces a cost that no profiler phase can
     see, suspect the subtraction before inventing a hiding place for the time.** And note the
     second trap inside the first: *"96.2% of REQUESTS served"* and *"% of BYTES served"* are
     different statistics, and here they differ by design — the pool takes the big streams, so
     it serves 97.1% of bytes while serving 96.2% of requests.

482. **WHEN A CHANGE REARRANGES ARGUMENTS RATHER THAN PIXELS, VERIFY THE ARGUMENTS.** Batching
     `vkCmdBindVertexBuffers` can fail exactly one way: a wrong `firstBinding` puts a REAL
     stream in the wrong binding. That is a plausible-looking mesh, no API error, and the
     existing order gate would pass it — it hashes the pipeline and the vertex RANGE, not
     which buffer landed in which slot. A picture aggregate on this route has a ~1.5% floor
     and answers a different question. So the gate recorded the `(binding, buffer, offset)`
     triples the draw ASKED for and compared them against an expansion of what the batched
     calls HANDED the driver — **0 disagreements over 335 M triples**, exhaustive, on every
     draw. **And design the poison so it fires on EVERY check.** The obvious one here —
     swapping two entries inside a run — could not fire on the 22% of runs that are one
     binding long, so it could never have read 100% and would not have shown the checker
     capable of failing (gotcha 30). Shifting every issued offset by a fixed amount does.

483. **A RUN THAT DIES IN THREE SECONDS STILL PRINTS A NUMBER.** Two arms of part 81's picture
     gate and the campaign's first run exited ~3 s after start — clean shutdown, no error
     string, **no `[fps]` line at all**, 863 log lines against a healthy run's 31,470 — and
     each still reported `peak windowed draws med: 0` through the normal gate path. The route
     gate caught them (that is what it is for, and it renamed them `.rejected`), but the
     shape is the lesson: **the failure mode of a launcher is a run that produces output**,
     not one that produces an error. Cause unknown as of part 81 and recorded as unknown.
     Check log LINE COUNT and the presence of the first `[fps]` line before believing any
     aggregate, and never let a glob pick up a trace whose log was rejected.

484. **A CWD-RELATIVE PATH IS A DEFECT THAT ONLY APPEARS ON SOMEBODY ELSE'S MACHINE.** Every
     asset path in this runtime resolved against the working directory — `"../../assets/game/
     default.xex"`, three `"../.."`-shaped shader-cache candidates, four for a config file —
     which is why every documented command begins with `cd runtime/build`. That is survivable
     for a developer reading the recipe and fatal for a player: a shortcut launch has a CWD of
     `$HOME`, finds no shader cache, and presents a black screen with one log line. **Anchor
     to the executable, print the root once at startup, and let nothing fall back to the CWD**
     — a CWD fallback is worse than none, because it keeps the development tree working while
     the shipped one silently does not, which is the one failure that survives every test done
     where the thing was built. `runtime/host/host_paths.h`.

485. **`ldd` CANNOT SEE A `dlopen`, AND A BUNDLE CHECKED WITH IT CAN STILL BE EMPTY.** The
     first Linux artifact passed its dependency check completely — every library resolved
     inside the bundle, nothing outside it but libc and the Vulkan loader — and then died on
     its first instruction in a clean container with `Failed loading SDL3 library.` Fedora's
     `libSDL2-2.0.so.0` is `sdl2-compat`, a shim implementing the SDL2 API by `dlopen()`ing
     `libSDL3.so.0` at run time. The dependency is real, it is fatal, and the tool the release
     plan specified for exactly this job is structurally blind to it — it worked on the build
     machine only because SDL3 was installed there. **A packaging gate must RUN the artifact,
     not inspect it**, and the container must lack the development packages or the test proves
     nothing. (Bundling the missing library does not rescue it either: the shim dlopens by
     soname and carries no RUNPATH, so the loader would still search the system directories.)
     Same shape as gotcha 25 — the instrument could not match, so its zero meant nothing.

486. **A "PERFECT DISCRIMINATOR" FOUND BY SCANNING IS USUALLY OVERFITTING.** Looking for the
     field that says how big a shader object's literal-constant block is, a scan of every
     header dword against the known answer reported one that separated the classes with no
     collisions at all — dword `@0x08`. It is the blob LENGTH, and it "worked" only because
     small blobs happen to have no constant block: 167 distinct values, none colliding, and no
     causal relationship whatever. The real field was one indirection away (`u32@(u32@0x18)`)
     and was invisible to any scan for a *value*, because it is reached through a pointer. It
     became obvious the moment the region between the header and the blob was DUMPED instead
     of scanned. **When a search over a value space returns a perfect answer, ask how many
     hypotheses it tried** — and read the structure before believing any of them.

487. **A GATE THAT CANNOT PROVE ITS OWN BODY RAN IS AN EXIT CODE, NOT A GATE.** The
     clean-container packaging gate printed `GATE PASSED` having executed nothing: `podman run`
     without `-i` does not attach stdin, so the here-doc went nowhere, `sh -s` read EOF and
     exited 0, and the wrapper reported success. That is gotcha 483 — a run that dies still
     reports a number — reproduced in a tool written the same afternoon that entry was read,
     which is how reliably this shape recurs. **Require EVIDENCE, not status**: the gate now
     insists on four marker lines that only code inside the container can print, plus the
     smoke harness's own success sentence.

488. **A CHEAP EXACT CLAIM BEATS AN EXPENSIVE STATISTICAL ONE — LOOK FOR ONE BEFORE MEASURING.**
     The release plan asked for the new build type to be measured on the crowd route, "because
     a build-type change is a performance change until measured". True in general, and on this
     workload it is three runs an arm and an hour against a 10-13% noise floor (gotcha 229).
     But `-g` does not affect code generation and `objcopy --strip-debug` does not touch
     `.text`, so the claim to test was BYTE IDENTITY: `sha256` of the extracted `.text`, one
     second, and it came back identical over 35,651,455 bytes. That does not merely suggest
     the frame is unchanged, it makes the change incapable of altering it. **The route run
     stays in the checklist as confirmation; it is no longer the evidence.**
     `tools/release_text_identity.sh`. Corollary found by the same tool: **a link option can
     move `.text` without changing an instruction** — a RUNPATH lengthens `.dynstr`, which
     sits before `.text`, relocating the image so that every address-bearing byte differs. Hold
     it matched rather than carving out an exception, because an exception is where a real
     difference hides.

489. **A 48-BYTE MATCH IS A PROLOGUE, NOT A SHADER.** `docs/release-plan.md` §1.4 reported 98.6%
     of this title's shaders recoverable from the disc banks, on the strength of a 48-byte HEAD
     probe against 1,571 disc objects. Measured by FULL containment: pixel shaders **343 of
     345**, vertex shaders **0 of 104**. Two different runtime vertex shaders match the same
     disc object at the same offset for exactly 48 bytes and then diverge — the probe was
     matching a shared vertex-shader preamble. The truth underneath is worth as much as the
     correction: aligned by their TAILS, disc and runtime vertex shaders differ in 3-35
     scattered bytes, always in groups of three dwords with whole fields zeroed on disc, which
     is the title patching vertex FETCH instructions at load out of the vertex declaration.
     **A probe length is a hypothesis about how much of a thing is distinctive**, and the way
     to test it is to ask how many distinct objects the probe matches, not how many it finds.
