# Phase 1 — kernel HLE and guest bootstrap

Written 2026-08-04 (session 4), while phase 1 is in progress. Status at the bottom.

The plan is `docs/runtime-plan.md` §"Phase 1"; the hand-off that started this work is
`docs/phase1-kickoff.md`. This file records what the work actually found — the parts
that were not in either document, because they came out of the ground truth or out of
running the thing.

---

## 1. The gate, and the two captures it needs

`tools/kernel_call_diff.py` compares the **first-occurrence order** of kernel calls,
ours against Xenia's. Not the raw stream: Case Zero calls `XamInputGetCapabilities`
5,501 times and `XamUserGetXUID` 4,441 times in the A1 boot, and no runtime
reproduces an emulator's thread interleaving. The order in which subsystems are
*first* touched is reproducible; the interleaving of the polls is not.

Three traps are baked into the tool because each one produces a confident wrong
answer:

**Xenia hides part of the kernel even at `log_level=3`.** Exports tagged
`kHighFrequency` need `log_high_frequency_kernel_calls=true`, which defaults off, and
A1–A4 all record it as false. A naive diff shows our runtime "calling functions Xenia
never called".

Case Zero's hidden set is **35 names**, and it is *measured* rather than copied from
the previous port: it is exactly `first-occurrence(A5) − first-occurrence(A1)`, A5
being A1's drive re-run with the cvar on. Deriving it this way makes it automatically
the intersection with what *this* title imports, and true to the fork the captures
were taken on. It is most of the synchronisation surface —
`RtlEnterCriticalSection`, `KeSetEvent`, `NtWaitForSingleObjectEx`,
`KeDelayExecutionThread`, `NtReadFile`, `VdSwap`.

**A5 is NOT a superset of A1.** It nearly is, which is what makes the assumption
dangerous. Eleven names appear in A1 and not in A5 — `XamShowDeviceSelectorUI`,
`XamContentAggregateCreateEnumerator`, `XamGetOverlappedResult`, `XamTaskSchedule`,
`XMsgInProcessCall` and the rest of the storage-device-selector path, which that
drive did not enter. So **A1 is the authority for the boot sequence, A5 for the
synchronisation surface, and neither alone is the gate.** Run both.

**Xenia's internal helpers wear the same line shape as kernel calls.**
`GetProcAddressByOrdinal`, `ResolvePath`, `SetThreadName`, `SetInterruptCallback`,
`XEnumerateCrossTitle`, `XamGetPrivateEnumStructureFromHandle` and `XGIUserSetContext`
are all logged as `Name(args)` and none is an import of this image. Both sides are
intersected with `ppc/ppc_recomp_shared.h`'s 244 `__imp__` externs.

## 2. The import set: three independent sources agree

Worth recording because it is the cheapest confirmation available that we are
building against the right title, and it cost one grep:

| source | functions | variables |
|---|---|---|
| `ppc/ppc_recomp_shared.h` (`PPC_EXTERN_FUNC(__imp__…)`) | 244 | — |
| A1's Xenia import dump (`F` / `V` rows) | 244 | 13 |
| our loader's descriptor walk at runtime | 244 | 13 |

`kernel/xex_imports.cpp` warns if its own walk disagrees with 244/13, so a future
drift is loud rather than silent.

**The 13 kernel *variable* ordinals are identical to Asura's Wrath's.** Same
ordinals, same names, despite a different studio, engine and SDK year. These are what
the XDK's static libraries pull in, so the set is a property of the SDK rather than of
the title — useful for Case West, and *not* a licence to skip the check.

## 3. The arena map is confirmed by this title, not assumed from the last one

Gotcha 9 says to put the guest arenas where the console puts them. A1 makes that
checkable in two independent ways:

```
NtAllocateVirtualMemory(base=00000000, size=00100000, 60002000, 4, 0) = 40000000
NtAllocateVirtualMemory(base=40000000, size=00010000, 60001000, 4, 0) = 40000000
NtAllocateVirtualMemory(base=40010000, size=00020000, 60001000, 4, 0)
...
NtAllocateVirtualMemory(base=00000000, size=00100000, 60002000, 4, 0)  <- second 1 MB
NtAllocateVirtualMemory(base=40100000, size=00020000, 60001000, 4, 0)
```

`0x60002000` = `MEM_16MB_PAGES | MEM_LARGE_PAGES | MEM_RESERVE`; `0x60001000` is the
same with `MEM_COMMIT`. So the title reserves 1 MB regions and then commits 64 KB
granules up through them. Two things follow and both are checkable rather than
assumed: the large-page arena must begin at exactly `0x40000000`, and the *second*
reservation must come back at `0x40100000` — which it does iff our arena hands out
64 KB-granular blocks in address order. It does.

The first physical allocation is a single **447 MB** `MmAllocatePhysicalMemoryEx`
(`0x1BF16000`), i.e. the title claiming essentially all of the console's 512 MB up
front. Our physical arena is 511.94 MB, so it fits with room to spare.

## 4. Findings

### Finding 15 — a generated stub cannot obey the out-parameter rule, and saying so is the fix

Asura's Wrath's finding 14 sharpened gotcha 5 into: *a stub for a call with an
out-parameter must either fill the out-parameter or not exist*, because "returns an
error" only protects you against a guest that checks the return.

`tools/gen_import_stubs.py` **cannot obey that rule**. It is produced from a list of
names in `ppc/ppc_recomp_shared.h` and has no signature, so it does not know which
register holds a pointer — and guessing by name prefix would be wrong *silently*,
which is worse than being wrong loudly. The resolution is not a cleverer generator; it
is to state the limit in the generator's own docstring, name the two failure modes so
they are recognisable in the field, and keep the escalation path short (give the
import a real `GUEST_FUNCTION_HOOK` signature in `kernel/imports.cpp`, and the
generator drops it on the next run).

The two failure modes, both of which look like something else:

- **A guest that trusts an untouched out-buffer.** Presents as corruption or a hang
  far from the call. The gate diff localises it: look at what our log called last.
- **A guest that uses the RETURN VALUE as a pointer.** Not every import returns
  NTSTATUS; some return handles, counts or pointers. `0xC0000002` is then a wild
  pointer, and the fault lands on a dereference of `0xC0000002` or an address near it.
  **That specific address in a segfault means "an unimplemented import was asked for a
  pointer", every time.**

### Finding 16 — an honest-failure stub steered the guest into its dirty-disc path, exactly as designed

The first run that entered the guest produced this:

```
  22  NtCreateFile               NtCreateFile
  23  NtQueryFullAttributesFile  XamShowDirtyDiscErrorUI
  24  NtClose                    XamLoaderLaunchTitle
```

`NtCreateFile` was still a generated stub, so the title concluded it could not read
the disc and took its dirty-disc path — a *correct* response to an honest failure. The
whole reason phase 1 converts the abort-stubs to honest failures is visible in one
line here: an abort would have stopped at call 1 and told us nothing about ordering,
whereas this diverged at a named call whose repair was obvious.

It also decided the phase boundary. `docs/runtime-plan.md` puts the VFS in phase 2,
but A1's **22nd** distinct kernel call is already an `NtCreateFile`, so phase 1's own
gate cannot be reached without a file layer. `kernel/vfs.cpp` and
`kernel/file_imports.cpp` therefore moved forward into phase 1; what stays in phase 2
is the part that phase is really about — the `.big` archive semantics and the
seek-order oracle.

### Finding 17 — a diagnostic can be disabled by an address-range assumption inherited from another port's memory map

Both template ports scan a stalled guest thread's stack for return addresses and bound
the scan with `addr < 0x80000000`. That is true of a console guest stack and true of
their runtimes'. It is **false of ours**: guest thread blocks are allocated from the
o1heap user arena, which `kernel/heap.cpp` places at `0x88000000`.

So the bound failed on the very first word, the scan broke immediately, and every
stall and wait trace printed

```
[stall] KeDelayExecutionThread tid=00000F00 ... lr=82829FCC callers:
```

with nothing after `callers:`. That reads as *"the instrument ran and found no guest
callers"* — a clean, small, wrong answer — rather than *"the instrument never ran"*.
It is gotcha 25's shape in a new place: before believing an empty result, confirm the
thing was capable of producing a non-empty one. Corrected, the same run prints ten
frames immediately.

The general form, and it is not specific to stack scans: **a ported diagnostic can
carry an assumption about the memory map that was true where it came from.** Every
inherited constant that is a *range* deserves the same question as every inherited
constant that is an *address*.

### Finding 18 — this title resolves all seven xam ordinals; Asura's Wrath's refusal does not transfer

A1's loader seam:

```
XexLoadImage("xam.xex", 9, 0, out) -> 30002000
XexGetProcedureAddress(30002000, 0xAFF)   XamPartyGetUserList
                       ... 0xB00          XamPartySendGameInvites
                       ... 0xB0B          XamPartySetCustomData
                       ... 0xB10          XamPartyGetBandwidth
                       ... 0x305          XamShowPartyUI
                       ... 0x30B          XamShowCommunitySessionsUI
                       ... 0x279
```

Seven resolutions, and Xenia resolves **all seven** — there is no "ordinal not found"
line anywhere in A1. Asura's Wrath's runtime reproduces a refusal (its ordinal 0x48C),
and copying that behaviour here would have invented a failure the console never had.
A title told "no party support" takes a different path from one told yes and then
handed an error at call time.

### Finding 19 — Case Zero leaves no free code-range constant, so the mint region is found rather than chosen

`XexGetProcedureAddress` must return a guest address the dispatch table can index,
i.e. inside `[PPC_CODE_BASE, PPC_CODE_BASE + PPC_CODE_SIZE)` — the table covers the
CODE range, not the image range. Asura's Wrath's finding 54 is what happens otherwise:
it picked a constant "past the last section, still inside the image", which was 4.7 MB
beyond the table, so the write landed 9.4 MB past the table's end and the stub that
was supposed to fail honestly never ran. The caller saw `r3 = 0` — success — from a
call that did nothing.

Case Zero offers no comfortable constant at all. Its last mapped function starts at
`0x829C3554` and the code range ends **16 bytes later** at `0x829C3564`. There is no
tail pad.

So the slot region is **found at first use**: one pass over the dispatch table for the
longest run of consecutive unmapped 4-byte slots, taking a point inside it, logging
what it picked and re-checking each slot at mint time. It costs one scan per process
and it cannot go stale, because there is no constant to go stale. That is the general
lesson — *when the safe value is a property of the generated function list, compute it
from the function list.*

### Finding 20 — the async completion is an APC, and a read that drops it looks like it worked

This is the one that unblocked the boot, and it took the prefix from **23 to 43**.

Our first `NtReadFile` filled the caller's buffer and its `IO_STATUS_BLOCK`,
returned `STATUS_SUCCESS`, and hung the title. The main thread polled a completion
flag forever while the `cAsyncFileSystem` worker sat in a wait, and nothing in our
log said anything was wrong — the read had *worked*.

A5 is the read oracle (finding 2 — `NtReadFile` is `kHighFrequency` and invisible in
A1), and one line settles it:

```
d> F8000018 NtReadFile(F8000020, event=00000000, apc=82831B21, apcCtx=82789670,
                       iosb=E418D208, buffer=FFCA0000, length=00009000, offset=0)
```

`event` is **zero**. This title's async file system delivers completion through the
**APC routine**, and the issuing thread then parks in an alertable wait so the APC
can run — A5 shows the idiom plainly on the same thread:

```
d> F8000018 NtSetEvent(F8000014, 00000000)                       <- its own queue event
d> F8000018 NtWaitForSingleObjectEx(F8000014, 1, alertable=1, NULL)
```

Our implementation signalled the event and ignored `apcRoutine` entirely. Signalling
a handle of 0 is a no-op, so the notification was simply dropped.

The general rule, and it is the out-parameter rule one level up: **the contract of an
async call includes its notification, not just its data.** A stub that gets the data
right and the notification wrong is *harder* to find than one that fails outright,
because every observable it touches looks correct.

Two supporting details that would each have cost time on their own: the APC routine's
low bit is the kernel's own flag rather than part of the address (`0x82831B21` →
`0x82831B20`), and the APC's `IO_STATUS_BLOCK` argument must be passed as a **guest**
address, not the host pointer the marshalling layer handed us.

A1 alone could not have found this. Its `NtReadFile` lines do not exist.

### Finding 21 — "fill your out-parameters" is only safe on top of a correct signature

`NtClearEvent` was written with `NtSetEvent`'s signature — `(handle, previousState)` —
and given an out-parameter fill for finding 15 compliance. It takes **one** argument.
So the fill read `r4`, a register the caller had left holding something else, and
stored through it: a SIGSEGV inside our own kernel, on a write the guest never asked
for.

The rule that produced the bug is a good rule. The lesson is that it composes badly
with a wrong signature, and in the worst way — it converts a harmless leftover
register into a wild store, in exactly the place the rule exists to protect. **Check
arity before adding an out-parameter write.**

A5 settles arity in one grep, because both calls are `kHighFrequency` and appear
nowhere else:

```
NtClearEvent(F8000020)              <- one argument
NtSetEvent(F8000014, 00000000)      <- two
```

The other seven out-parameter hooks were audited against the captures the same way and
all were already right.

### Finding 22 — the GPU device-struct offsets are per-title, and this image states its own

The pump needs two words out of the driver's device struct: the mirror of the kicked
write pointer, and the pointer to the read-pointer write-back slot. Asura's Wrath
reads them at `+11088` and `+11024`. **Case Zero's are at `+10956` and `+10896`** —
copying the previous port's numbers would have read arbitrary struct fields as a ring
position.

The image states both. There is exactly ONE store to `CP_RB_WPTR` in the whole image
(the only `PPC_MM_STORE_U32` to `0x7FC80714`), in `sub_82845698`:

```
stw  r11,10956(r29)     ; the mirror
sync
lis  r10,32712          ; 0x7FC8
stw  r11,1812(r10)      ; CP_RB_WPTR
eieio ; sync
```

and the free-space wait that blocked the boot dereferences the other:

```
lwz  r11,10896(r31)     ; the write-back slot's address
lwz  r11,0(r11)         ; how far the GPU has consumed
```

Both are now confirmed live: with `CZ_RING_TRACE=1` the mirror and the MMIO register
read back **identical** (`kickedWptr=000031E4`, `mmio CP_RB_WPTR=000031E4`), which
turns the "use the mirror, not the register" advice inherited from Fable 2's finding
48 into something checked here rather than believed.

### Finding 23 — the ring-buffer size argument, derived from the guest's own arithmetic

`VdInitializeRingBuffer(03D72000, 14)`. The second argument is not a byte count and not
a plain log2. The guest computes it immediately before the call:

```
cntlzw r11,r25        ; clz(ring size in BYTES)
subfic r23,r11,28     ; 28 - clz(size)
```

For a power-of-two size S, `clz(S) = 31 - log2(S)`, so the argument is
`log2(S) - 3 = log2(S/8)` — the log2 of the ring size in **quadwords**, and
`size = 1 << (arg + 3)`. Case Zero passes 14, so the ring is 0x20000 = 128 KB, which A1
confirms independently: the `MmAllocatePhysicalMemoryEx` immediately before it is for
exactly 0x20000 bytes. A factor-of-8 error here is silent until the ring wraps.

### Finding 24 — Xenia's physical addresses carry a +0x1000 skew, and it invalidates a naive geometry check

Chasing the ring size produced an apparent contradiction: with
`physical = virtual & 0x1FFFFFFF`, the ring starts 0x1000 inside its own allocation and
overruns the end by the same 0x1000. That is not a guest bug and not a formula error.
**Xenia's `MmGetPhysicalAddress` does not simply mask.** A1:

```
MmAllocatePhysicalMemoryEx = E3D71000 Size: 00020000
MmGetPhysicalAddress(E3D71000)
VdInitializeRingBuffer(03D72000, 14)      <- the answer, on the very next line
```

`(0xE3D71000 & 0x1FFFFFFF) + 0x1000 = 0x03D72000`. The same +0x1000 reproduces the
write-back slot exactly: `(0xE3D70000 & 0x1FFFFFFF) + 0x1000 + 0x3C = 0x03D7103C`. With
the skew the ring starts at its allocation base and fits exactly.

Our own convention needs no skew — `MmGetPhysicalAddress` masks and `PhysicalToVirtual`
ORs back, which round-trips — but **a physical address in our log is 0x1000 below the
same object's address in a capture**, and any geometry argument that mixes the two is
wrong. This is a sharper version of the claim in `kernel/heap.h` that matching the
console's map makes our addresses "directly comparable to a capture's": true for
virtual addresses, false for physical ones.

### Finding 25 — the crash reporter, and what it found on its first crashing run

`runtime/cpu/crash_report.{h,cpp}`: SIGSEGV/SIGBUS/SIGILL/SIGTRAP handlers that print
the **guest** state — faulting guest address and which arena it lands in, guest thread
id, the full GPR file, an exact LR back-chain backtrace, and the host pc to
`addr2line`. It exists because the fault it was written for happens in a minority of
runs, which is the worst case for attaching a debugger after the fact, and because
gdb on this 109 MB binary takes minutes just to load symbols.

It named a bug on the first run that crashed:

```
=== guest fault: signal 11 at host address 0x7f0cce45e01a ===
the faulting address is OUTSIDE the 4 GB guest space
guest thread id 00000F28
lr=82844D6C ctr=0BADF00D r1(sp)=88155B80 r13(pcr)=88114D60
r3  00000200   r10 0BADF00D   r11 BBF39340
[r11] BBF39340: 00000000 00000001 00000000 00000000 0BADF00D 00000200 ...
```

`ctr = 0x0BADF00D` is a poison value, and `[r11+16]` shows where it came from. The
guest's graphics ISR (`sub_82844D38`) has a four-instruction source-1 path:

```
lwz  r11,10900(r4)    ; the scratch-register mirror
lwz  r10,16(r11)      ; SCRATCH_REG4 = a callback pointer
cmplwi cr6,r10,0
beq  <skip>           ; ZERO means nothing armed
lwz  r3,20(r11)       ; SCRATCH_REG5 = its argument
mtctr r10 ; bctrl
```

It checks the callback against **zero and nothing else**. The stream writes
`0x0BADF00D` there once a callback has been consumed, and on hardware the ISR can
never see that, because the command processor stalls at the `WAIT_REG_MEM` that
follows until the CPU has serviced the interrupt. Our executor evaluates that wait
once and carries on (`gpu/pm4.cpp` explains why blocking would deadlock against the
very thread that has to satisfy it), so our CP can run ahead of the CPU-side handshake
and reach a later INTERRUPT with the mirror still poisoned.

`docs/runtime-plan.md` predicted this shape before any of it was written — *"a packet's
contract can include when it runs relative to its neighbours"* — and it is worth noting
that delivering **in-position** (from inside the walk, at the INTERRUPT packet) was
already done and is not sufficient on its own.

The response is `MirrorIsPoisoned()` in `gpu/vd.cpp`: decline that one delivery,
counted and logged. That is not faking anything — the stream has explicitly marked the
callback dead, and the interrupt we would deliver is one the console would not have
delivered at that point. **It has not fired since**, which is recorded here so nobody
mistakes it for load-bearing; it guards a state observed once.

`CZ_PM4_STOP_ON_WAIT=1` is the more faithful alternative — stop the ring walk at an
unsatisfied wait and resume next tick, which is what hardware does — offered as an
off-by-default arm because a wait satisfied only by later work in the same stream
would stall the ring permanently. Measured: **no difference in crash rate**, so it is
unproven either way and stays off.

### Finding 26 — RETRACTION: "~2 runs in 10" was wrong, and there are two crashes, not one

The previous session recorded the intermittent fault as ~2 runs in 10 and blamed the
new pump thread. Both halves need correcting.

**The rate is 6-7 in 10 at 20 s**, not 2. The earlier figure came from one batch of ten
runs and did not survive re-measurement. An intermittent failure needs its rate
re-established whenever anything around it changes; a number measured once is a
number about that afternoon.

**There are at least two distinct faults.** The poison one above is on the pump thread
(`tid 00000F28`). The dominant one is on the **main thread** (`tid 00000F00`) and is a
null-pointer walk, which the crash reporter separates cleanly by re-running the same
binary under the three page-0 policies `kernel/memory.cpp` already provides:

| page 0 policy | crashes | faulting guest address |
|---|---|---|
| `PROT_NONE` (bring-up default) | 6/10 | `00000000` |
| `PROT_READ` (null reads succeed, as on console) | 6/10 | `00000002` |
| read/write (console behaviour) | 7/10 | outside the 4 GB space |

That progression is the diagnosis. It is **not** a benign console-tolerated null read
(Fable 2's finding 63): under `PROT_READ` the read at 0 succeeds and it faults two
bytes further on, and with page 0 fully mapped it runs off into a wild host address.
The guest is walking a base pointer that should not be null — so something we return
is null where an object is expected, which is finding 15's first failure mode
(a guest trusting an out-parameter, or a zero-returning stub).

The 17-frame back-chain is in hand and rooted at the boot path
(`825DA0C0 -> 825D7564 -> 825D2648 -> ... -> 82956678`), which is where the next
session starts.

### Finding 27 — the null base pointer was not a null pointer: an unlowered `bctr`

RESOLVED. And the diagnosis in finding 26 was wrong in an instructive way.

Finding 26 concluded the guest was "walking a base pointer that should not be null —
so something we return is null where an object is expected", and named finding 15's
failure mode (a zero-returning stub, or an out-parameter we never filled) as the prior.
That prior was wrong. **No kernel call was involved at all.** The null came from the
recompiler.

#### What the crash actually was

Six crashing runs turned out to be **byte-identical** — same registers, same host pc,
same stack pointers. That single observation reframed everything: the fault is fully
deterministic in content, and only *whether the boot reaches it within 25 s* varies. An
intermittent-looking crash need not be an intermittent bug.

`addr2line` on the host pc named the faulting instruction exactly:
`ppc_recomp.206.cpp` line 6218, `lvx128 v8,r0,r8` in `sub_829565B8`, where
`r8 = [r31+4]`. Note that neither `r3` nor `r4` in the crash dump was null — the
register file for the innermost frame is partly stale, because the compiler keeps
`PPCContext` fields in host registers between calls. **The host pc is the only
authoritative register in the report.** The same staleness put one extra
already-returned frame at the top of the guest back-chain (`ctx.r1` still held
`sub_82955780`'s frame), which is worth knowing before reading any future one.

#### Finding the producer

`sub_829565B8`'s arg0 is a five-word descriptor its caller `sub_82959360` builds in
its own stack frame, and the faulting load reads `desc+4`. A probe on the alias seam
(`runtime/cpu/guest_probe.cpp`, `CZ_ARG_PROBE=1`) showed `desc+4` was a **valid**
pointer when written. So it was destroyed between the write and the read, by one of
the three calls in between — and watching `r31` and the memory word across each call
named the culprit in one run:

    [watch] sub_82955780   in    r31=88040DE0  [desc+4]=A434E9F0
    [watch] sub_82955780   out   r31=88040BC0  [desc+4]=A434E9F0

The memory was untouched. **`r31` was not restored.** `sub_82955780` contains a `bctr`
that XenonRecomp had not lowered to a `switch`, so it emitted
`PPC_CALL_INDIRECT_FUNC(ctr); return;` — a call to the case body followed by a return
with no epilogue. `sub_829565B8` resumed with `sub_82955780`'s `r31`, read `[r31+4]`
out of an unrelated object, got zero, and loaded a matrix through it.

The defect class, the scanner gap that caused it and the detector that now measures it
are written up in `docs/xenia-capture-analysis.md` §15.

#### The second layer: the coverage oracle hid it

The missed table should have crashed *at the dispatch*. `PPC_CALL_INDIRECT_FUNC` on a
case-label address normally finds nothing and jumps to null, which the crash reporter
recognises by name. It did not, because the coverage oracle had already added those
case labels as function starts (gotcha 21) — so `sub_82955EF8` existed, a 10-instruction
fragment with no prologue and no epilogue. The dispatch resolved, the body computed the
right answer using the caller's context, and the only trace left was the corrupted
register file.

**A tool that recovers missing entry points can convert a loud failure into a silent
one.** Gotcha 21 already said adding a case label splits its parent; this is the other
half — it can also make a *missed table* appear to work.

#### Result

Both defect sites are fixed, the pipeline is clean, and the effect is measured:

| | before | after |
|---|---|---|
| crashes in 10 runs at 25 s | 6-7 | 1 in 20 |
| switch tables | 232 | 234 |
| unlowered switch defects | 2 (unmeasured) | 0 |

The one surviving crash in 20 is a **different** fault: guest thread `00000F2C`, not the
main thread, faulting on an address outside the 4 GB guest space with
`lr=8284B708 / 82829BEC` and `0xC0000102` visible in the object at `r3`. That is the
next lead, and it is not this one.

### Finding 28 — one stubbed *query* was steering the whole frontend boot

Gate position 57 had been "the XAM/frontend surface, a big pile of unimplemented
imports" since the gate existed. Most of it was one value.

`XamGetSystemVersion` returns the dashboard version as a packed dword, and Case Zero
uses it as a **feature gate** in seven places. Every site is the same shape — an
unsigned `cmplw` against a constant, taking the older-system branch below it:

| call site | threshold | call site | threshold |
|---|---|---|---|
| `825D7E68` | `0x20096B00` | `825F209C` | `0x200CE900` |
| `825DFB34` | `0x200A3200` | `825F2218` | `0x200CE900` |
| `825DFD28` | `0x200A3200` | `828A0F2C` | `0x0008A100` |
| | | `828A1004` | `0x0008A100` |

Above the threshold the title resolves newer XAM entry points **dynamically** —
`XexGetModuleHandle` then `XexGetProcedureAddress(handle, ordinal)` — and calls
whatever comes back. Below it, it calls the statically imported function.

As a generated stub this returned `STATUS_NOT_IMPLEMENTED` = `0xC0000002`, which
compares **above every one of the seven thresholds**. So we took the dynamic path at
all seven sites, asked for ordinal 80 out of an export table we do not have, got
nothing, and silently lost the feature. `sub_825DFB10` is the `XNetStartup` wrapper
and `sub_825DFD28` the `WSAStartup` one; both were affected, which is why our boot had
no `NetDll_WSAStartup` at all.

Returning **0** is not a guess. It is the truthful statement "this system does not have
those newer XAM entry points" — claiming a higher version would be exactly the faking
success gotcha 5 forbids, since the title would then ask us for entry points we cannot
supply. It also matches the ground truth: in A1 the title takes the static branch and
calls `NetDll_WSAStartup(1, 0002, ...)` directly, with no `XexGetModuleHandle` within
a hundred lines. **Raise this only together with a real XAM export table, never before.**

Effect on the gate: `XexGetModuleHandle` moved from our position 59 to 70 (hardware
has it at 72), the bogus `XexGetProcedureAddress(ordinal 80)` is gone, and positions
58-61 now align with hardware.

#### A predicate-shaped import has no honest failure value

`RtlCompareStringN` was also a generated stub returning `0xC0000002` — and that is a
case "fail honestly" does not cover. The guest tests the result with `cmpwi r3,0`, so
`0xC0000002` is not an error it can notice; it is a perfectly valid **answer** meaning
"not equal". Every string comparison in the title silently returned "different", with
total confidence and no diagnostic anywhere.

This is gotcha 5's blind spot: the rule assumes the caller can distinguish a status
from a result. For an import whose return value is a *predicate* or a *comparison*,
there is no value that means "I don't know", so the only correct option is to implement
it. It is now implemented, with the argument shape read off the guest
(`s1, len1, s2, len2, caseInsensitive`) rather than assumed.

#### What is still at position 57, and what it is not

The gate still diverges at 57, and it is now a well-understood *extra* call rather than
a mystery. Our boot enters the title's **DVD-cache subsystem** and hardware does not:

    NtCreateFile('\Device\Image')                 -> not found
    NtCreateFile('\Device\Harddisk0\partition0')  -> not found
    RtlCompareStringN            <- gate position 57

`sub_82822638` is the predicate behind it: given a counted string it compares the
prefix against `"d:"`, a 5-character literal and `"cdrom0:"` (0x820BC4C0) — i.e.
"is this path on the disc?". The string table it lives in is unambiguous about the
subsystem: `\Device\Image`, `default.xex`, `cdrom0:`, `cache:\$cache$\spc`,
`DvdCache`, `\Device\Harddisk0\Cache%u`, `xbdm.xex`.

The branch that decides it is at **0x827890B4** in `sub_82788F48`: when
`sub_82829098(...)` returns non-zero the whole block — including the `sub_82827318`
call that reaches the comparison — is skipped. Hardware skips it; we do not.

> **RETRACTED — see finding 33.** Everything above is accurate and none of it is the
> cause. 0x827890B4 is a symptom two levels down: the real branch is 0x82789080, one
> call earlier in the same function, testing a stubbed **`XamContentGetLicenseMask`**.
> Naming 0x827890B4 "the branch that decides it" sent the next session looking at a
> DVD-cache subsystem that was behaving correctly throughout. The `cache:` symlink
> hypothesis below is also dead — it was never tested because it never needed to be.

Worth noting for whoever picks this up: Xenia's own config registers `cache:`,
`cache0:` and `cache1:` symlinks (`mount_cache = true`), and our VFS has no cache
device at all. That is a real difference in the environment the two runs see, and it
is the first thing to test — but it is a hypothesis, not a finding: A1 contains no
`NtCreateFile` on any `cache:` path, so hardware never actually opens one.

### Finding 29 — the network and profile block, and where its answers came from

Eleven imports implemented; gate positions 72-79 now match hardware exactly and the
boot reaches six kernel surfaces it had never touched (`NetDll_WSAStartup`,
`XamUserGetSigninInfo`, `XMsgStartIORequest`, `KeResetEvent`, `NtSetInformationFile`,
`XMACreateContext`).

The point of this entry is not the list, it is **where each return value came from**,
because the capture does not contain any of them. Xenia logs an import's arguments on
entry — pointer arguments as `ADDR(value)`, showing what was there BEFORE the call —
and prints no `= result` line for a single XAM or NetDll export. So A1 gives arity,
argument shapes and call order; the image gives the answers. `tools/import_call_sites.py`
exists to make that lookup cheap.

#### The network init is gated on one comparison

`sub_8280D748` is the whole story:

    bl   sub_825DFBD0          ; XNetStartup(1, params)
    cmpwi r3,0
    beq  loc_8280D7B0          ; 0 -> on to WSAStartup(0x202, &wsadata)
    bl   sub_825DFBE0          ; anything else -> tear down, skip the block

As an honest-failure stub `XNetStartup` returned `0xC0000002`, so the title dismantled
its network stack on every boot and never called `WSAStartup` at all. Returning 0 does
not claim a network exists; it claims the socket layer initialised, which is the only
thing the title can observe at that point.

Two out-parameter sizes worth recording, both read off the guest rather than the SDK:

* **WSADATA is 398 bytes.** The guest proves it — at `0x8280D7C8` it stores one
  halfword at `r1+96` and memsets **398** bytes at `r1+98` before passing `r1+96`.
  That is `2 + 2 + 257 + 129 + 2 + 2 + 4`, the classic Winsock layout.
* **`XNetGetTitleXnAddr` must not return 0.** `XNET_GET_XNADDR_PENDING` *is* zero, and
  `mr. r31,r3; beq` at `0x8280D7FC` sends the title back to ask again — a poll with no
  exit. `XNET_GET_XNADDR_NONE` (1) says "the answer is final and there is no address",
  which is both true here and terminating. `sub_825C6DA0` and `sub_8280D970` then test
  the DHCP/PPPoE/STATIC/ETHERNET bits individually and take the no-link path.

`XNetRandom` uses a **fixed-seed** LCG on purpose. Everything in this project is
measured by re-running the same binary and diffing; an import injecting host entropy
makes two runs legitimately different for reasons invisible in a log.

#### One local user, and the capture chose it

The runtime presents exactly one user: index 0, signed in **locally**, no online
identity. That is a policy decision and it is A1's: the capture runs
`XamUserGetName -> XamUserGetSigninInfo -> XamUserGetName -> XamUserReadProfileSettings
-> XamUserCheckPrivilege`, which is the flow of a title that found a profile. Reporting
"nobody is signed in" is defensible in isolation but is not what the ground truth shows,
and everything past it would be unfalsifiable.

The sharpest piece of evidence in the whole block is the signin-info XUID:

* **The out parameter is EIGHT bytes.** `sub_825C2F88` is the only consumer in the
  image; it zeroes an 8-byte slot with `std r11,80(r1)`, passes `r1+80`, and reads it
  back with `ld r3,80(r1)`. Writing the SDK's larger `XUSER_SIGNIN_INFO` there would be
  writing past what the caller reserved, on a layout nothing here confirms — exactly
  the failure gotcha 48 was written for.
* **Which XUID, decided by A1's sequence.** A1 calls it twice: `(0, 00000001, ...)`
  then `(0, 00000000, ...)`. The guest only makes the second call when the first
  returned a *zero* XUID (`cmpldi cr6,r3,0; bne cr6,<done>`). So on hardware `flags=1`
  asks for the ONLINE xuid and there is none. We reproduce that, which is both true of
  us and what makes our call sequence match.

`XamUserCheckPrivilege`'s out is a 4-byte BOOL (`lwz r11,96(r1); cmpwi cr6,r11,1`), and
A1 asks for privilege `0xFC` = `XPRIVILEGE_COMMUNICATIONS`. With no Live identity the
truthful answer is "not granted".

The gamertag is a **free choice** and is flagged as one in the source: it is not
observable in any capture, and every consumer of `XamUserGetName` just copies the
string out without branching on it.

#### What is still wrong in this window

Position 57 is unchanged — the DVD-cache fallback of finding 28, which shifts
everything after it by one. Beyond that, three real differences remain in the block:
we do not reach `NtCreateTimer` / `NtSetTimerEx` / `XamUserCheckPrivilege` (hardware
positions 69-71), and we call `XMsgCancelIORequest` and `KeQueryBasePriorityThread`
where hardware calls neither. `XamUserReadProfileSettings` is still a stub and is the
obvious next domino: a title that cannot read its profile settings cancelling an XMsg
IO request is a coherent story, but it is a story, not a measurement.

### Finding 30 — the profile-settings layout, derived from the guest and checked against two sizes

`XamUserReadProfileSettings` was the domino past the user block. Until it worked, the
title's frontend tore itself down and called `XMsgCancelIORequest`, which A1 never
does there — a call we make and hardware does not, which gotcha-wise is the most
informative kind of gate divergence there is.

The problem is that **the capture contains no return value and no post-call buffer for
any of this**. Xenia logs an import's arguments on entry, pointer arguments as
`ADDR(value-before-the-call)`, and prints no `= result` line for any XAM export. So
the layout of a structure the kernel is supposed to *fill* is exactly the thing the
ground truth cannot show. Two other witnesses can.

**The first witness is arithmetic the title does in public.** A1's calls come in
pairs: a query with a null buffer, then a real read using the size the kernel wrote
back.

```
ReadProfileSettings(FFFE07D1, FF, 0,0, 3, 829E05C8, 4017B400(00000000), 00000000, 00000000)
ReadProfileSettings(FFFE07D1, 00, 0,0, 3, 829E05C8, 4017B400(00000080), 4017B530, 4017B3E0)
ReadProfileSettings(00000000, 00, 0,0, 2, 7018F068, 7018F060(00000058), E7367A00, 00000000)
```

3 settings → 0x80 and 2 settings → 0x58. In integers that has one solution: an 8-byte
header plus 40 bytes per setting. Two independent points, and — worth saying, because
it is what makes the flat arithmetic safe — neither leaves room for a variable-length
payload, so this title only asks for fixed-size settings at boot.

**The second witness is the guest's own walk of the result.** `sub_825E4E88` reads the
buffer back and names every offset that matters:

```
count = [results + 0]                   header +0  = setting count
for i in 0..count:
    p  = [results + 4] + i*40           header +4  = pointer to the array
    id = [p + 16]                       setting    +16 = setting id
    switch (id - 0x1004000C) ...        VOICE_MUTED / _THRU_SPEAKERS / _VOLUME
    case 2: v = [p + 32]                setting    +32 = the value
            if (v > 100) v = 100        ... and it is a 0..100 volume
```

`+16` and `+32` with a stride of 40 pin the whole record: `from` at +0, the
xuid/user-index union at +8 (8-aligned, hence a pad at +4), the id at +16, and an
`X_USER_DATA` at +24 whose type byte is at +24 and whose 8-byte value union is at +32.
That is the same layout Xenia uses. The point is not that we agree with Xenia — it is
that **the title stated it**, so the agreement is a check rather than an assumption.

Three smaller things the guest decided for us:

- **The return values are not free.** `sub_825E5D28` tests `cmplwi cr6,r3,122` and
  *requires* `ERROR_INSUFFICIENT_BUFFER` from the query call; a success there sends it
  to the error path. `sub_825E4E88` accepts `0` or `997` from the read. So a query
  must fail and a read may report itself pending.
- **`userIndex == 0xFF` is not an error.** All four query calls pass it, meaning "no
  particular user". Answering `NO_SUCH_USER` fails the size query and the read never
  happens.
- **The value type is in the id.** The top nibble of a setting id is its
  `X_USER_DATA_TYPE` (`0x1004000C` → INT32). Deriving it means a setting we have never
  seen still gets a well-formed record, instead of a hand-maintained table silently
  returning the wrong shape for id number twelve.

**The overlapped structure came out of the title's own `XGetOverlappedResult`.**
`sub_825D83A8` reads `[ovl+0]`, compares it against 997, waits on `[ovl+12]` when it
is still pending, and returns `[ovl+4]` as the transferred length — so +0 is the
result, +4 the length and +12 the completion event, stated by the consumer. Our
`CompleteOverlapped()` fills all three and signals the event even though this
particular consumer will never wait on it, because gotcha 46 is precisely about the
notification half of an async contract being the half that gets dropped. What it does
NOT do is dispatch a completion routine at +16; no Case Zero boot path sets one, and
the code says so with a warning rather than by staying silent.

**Effect on the gate:** `XamUserReadProfileSettings`, `NtCreateTimer`, `NtSetTimerEx`
and `XamUserCheckPrivilege` all now appear where hardware has them, and both
`XMsgCancelIORequest` and the early `KeQueryBasePriorityThread` disappeared from the
window.

**One difference left in this block, recorded rather than papered over.** A1 calls
`XamUserCheckPrivilege` exactly once, for `0xFC` (COMMUNICATIONS). The guest only
stops there if the call *failed* or the privilege was *granted*; ours returns success
with "not granted", so we go on to ask about `0xFB` (COMMUNICATIONS_FRIENDS_ONLY),
which hardware does not. That is a consequence of our no-Live policy being stricter
than the capture environment's, it is invisible to a first-occurrence gate, and
inventing a granted communications privilege to hide it would be claiming an online
identity we do not have.

### Finding 31 — the kernel version was a free constant until the title branched on it

`xex_imports.cpp` published `XboxKrnlVersion` as 2.0.14448.0, inherited from both
template ports, with a comment saying that if Case Zero ever branched on it that would
be a finding rather than a constant to quietly tune. It does.

`sub_825D7AC8` — the rumble path — loads the version struct and takes a legacy branch
only when `major == 2 && minor == 0 && build < 5611`. Inside that branch it calls
`XamInputGetCapabilities`, and if the device reports sub-type 2 with capability flags
`0b11` it passes the two motor speeds to `XamInputSetState` **swapped** (`lhz r11,2(r31)`
stored at +0, `lhz r10,0(r31)` at +2) — a workaround for one accessory whose motors are
reversed. A1's Xenia config line is `kernel_build_version = 1888`, so the capture takes
that branch and 14448 does not.

The value is now 1888, and the reason is faithfulness, not performance — **measured, so
the claim is not oversold**: three 25 s runs at each value reach 82/85/82 and 85/82/85
visible kernel calls. Same distribution; the 82-vs-85 spread is boot timing. Gotcha 50
again, in its cheapest form — had we run one arm each we would have "shown" a 3-call
improvement that does not exist.

It is also the conservative direction. A version number is a claim about which XAM
entry points exist, ours is a minimal XAM, and gotcha 58 says raise a version gate only
together with the exports it unlocks.

### Finding 32 — a predicate stub was answering "yes" ten thousand times a boot

`XNotifyGetNext(listener, matchId, &id, &param)` is a BOOL. All five call sites test it
with `cmpwi r3,0; beq skip`. As a generated honest-failure stub it returned
`STATUS_NOT_IMPLEMENTED` — `0xC0000002`, which is not zero — so **every poll told the
title a notification had arrived**, and the title then read the id and param out of two
stack slots the stub had never touched. A5 shows the real thing polling a single
listener 10,480 times in one boot; ours was manufacturing that many phantom events out
of stale stack, silently, with no fault and no log line.

This is gotcha 59 a second time and it is worth stating as a rule rather than an
anecdote: **the "fail honestly" doctrine has a blind spot wherever the return value is
a predicate**, because there is no bit pattern in a boolean that means "I could not
answer". `RtlCompareStringN` (finding 28) was the first; this is the second; a third
will exist. The escalation is not a better stub, it is a real implementation.

It is also gotcha 42 from the other side: the out-parameters have to be written on the
FALSE path too, since that is the path taken thousands of times a boot and the caller
reads them regardless of the return.

**What we post: nothing.** An empty queue is the truthful statement that nothing has
happened since boot, and it is the only one we can currently support — there is no
input layer, no storage layer and no system UI to generate an event.
`PostGuestNotification()` is the seam those layers will use, and it is written now
rather than deferred because a queue nothing can fill is a queue nothing can test. The
ids to raise when a source appears are readable in `sub_825E4380`, which compares the
delivered id against 10 and 14 — `XN_SYS_SIGNINCHANGED` and
`XN_SYS_PROFILESETTINGCHANGED`.

**The controller, in the same session and under the same policy as the single local
user:** player 1 holds a standard wired gamepad, players 2-4 hold nothing and say so.
A1 polls `XamInputGetCapabilities` 1,108 times for user 0 *and* 1,108 times for user 1,
so "not connected" is an answer this title is built to receive constantly rather than
an error path. The pad reports neutral because there is no host input source yet — that
is a missing feature, not a claim that no buttons are pressed, and the difference is
worth being explicit about. It costs nothing today: the title sits at its press-start
screen either way, and this is the shape SDL plugs into.

One deliberate non-match, from finding 31's branch: we report sub-type 1, not the 2
that `sub_825D7AC8` tests for. Reporting 2 would claim to be the accessory with
reversed motors and get our rumble inverted for the sake of matching a comparison.

### Finding 33 — the position-57 divergence was a licence query, not a disc

Gate position 57 had been open since the runtime first booted. It is closed, and the
way it closed is worth more than the fix, which is four lines.

**The visible trail was entirely real and entirely innocent.** Our boot did this:

    NtCreateFile('\Device\Image')                 -> not found
    NtCreateFile('\Device\Harddisk0\partition0')  -> not found
    RtlCompareStringN                             <- gate position 57

and the string table around the comparison is unambiguous about the subsystem —
`\Device\Image`, `cdrom0:`, `cache:\$cache$\spc`, `DvdCache`,
`\Device\Harddisk0\Cache%u`. Finding 28 read that correctly, identified
`sub_82829098` as the DVD-cache initialiser, found the branch at `0x827890B4` that
consumes its result, and named that as the deciding branch. All true. None of it the
cause.

**Printing the predicates beat reading them.** `sub_82829098` has four separate routes
to a zero return and two of them are *failure* paths, so a static read of the polarity
kept flipping. Probing the returns settled it in one run:

    [ret] sub_82823A58   -> 1    '\Device\Image' is absent — correct, this is not a disc title
    [ret] sub_82831528   -> 2    ERROR_FILE_NOT_FOUND on \Device\Harddisk0\partition0
    [ret] sub_82829098   -> 0    ... and 0 is what makes the caller run the cache block

**Then the capture said the whole trail was unreachable.** A1's first `NtCreateFile`
is `game:\layout.bin`. Hardware never opens `\Device\Image` at all — so it never
enters `sub_82829098`, and the deciding branch could not be the one that consumes its
result. It had to be earlier.

One call earlier, at `0x82789080`:

    sub_825D7A50()            // literally `b XamContentGetLicenseMask` — a tail-call thunk
    cmplwi r3,0
    beq    loc_82789134       // SUCCESS -> skip everything below
    ... sub_82829098 ...      // FAILURE -> go looking for a disc

`XamContentGetLicenseMask` was a generated honest-failure stub returning
`0xC0000002`. The title asked "am I licensed?", could not get an answer, and did the
reasonable thing: went to check whether it was running from a disc.

**Why it was invisible.** `XamContentGetLicenseMask` is `kHighFrequency`. It appears
**nowhere in A1** and three times in A5, which is where the argument shape and the
null overlapped came from. Gotcha 47, paid in full: a capture set needs a
high-frequency arm or its quietest exports are unfalsifiable. And a one-instruction
tail-call thunk means the import does not appear in the calling function's own
disassembly under its own name — `sub_825D7A50` is what you see.

**The mask value is finding 1 arriving in the runtime.** A second call site, at
`0x8250191C`, decides trial-versus-full with the value rather than the status:

    r11 = [r1+80]            // the mask we wrote
    r9  = (mask == 0)
    [0x82505FFE] &= r9       // a global byte, cleared when the mask is non-zero

Zero mask leaves the flag standing; non-zero clears it. That is the ledger's finding 1
from the inside — Xenia's `license_mask` defaults to 0 and boots the **trial**, and
both A1 and A5 were captured with `license_mask = 1`. Our package is the full game, so
bit 0 is set. This is the one place in the runtime where a wrong value would silently
boot a different game rather than fail.

**Also fixed while here:** `XexCheckExecutablePrivilege` was `return 0` for every
privilege — a constant standing in for a question the image answers. It was right by
luck (Case Zero's `XEX_HEADER_SYSTEM_FLAGS` is `0x00000200`, bit 9
`TITLE_USES_GAME_VOICE_CHANNEL` and nothing else, and the three privileges this title
asks about are all clear), which is not a reason to keep it. It now reads the header.
Case West's flags will differ and the failure mode is a silently wrong branch.

**Result.** Position 57 is gone. The A1 gate is now a clean prefix match through
position 70, and one run in four is an **exact 81-deep prefix of Xenia's 93** with no
divergence at all — the first time this port has produced one. The remaining mismatch
in 71-76 is a pure permutation: both sides hold the same six names, `XamUserCheckPrivilege`
lands first on hardware and last for us, and it is not stable across our own runs, so
it is a thread race rather than a defect.

**The transferable lesson, and it is about instruments rather than licences.** A probe
answers the question you point it at. Every reading in that trail was accurate, and
following it downward produced ever more detailed confirmation of a symptom. What
found the cause was walking **outward** — to the first caller whose behaviour differs
from the capture — and the cheapest form of that question is not "why did this fail"
but "does hardware even get here". A1 answered it in one grep.

### Finding 34 — the XAM message/task block, and the domino that is not storage

Nine imports, written as one mechanism rather than nine entries, because they are one:

- **`XMsgStartIORequest(app, message, overlapped, buffer, len)`** is a *dispatcher*,
  not a leaf. A blanket error from it fails every XAM message the title will ever
  send, which is what the generated stub did.
- **`XamTaskSchedule(callback, context, 0, handleOut)`** runs a **guest** function on
  a worker thread. This is the load-bearing one and it is easy to mistake for
  bookkeeping: A1's `XamTaskSchedule(825D9358, 300E9000, ...)` schedules
  `sub_825D9358`, and that function is the *only* caller of `XMsgCompleteIORequest`
  in the entire image. Stub the scheduler and the title's async operations are
  started and never finished — gotcha 46's shape, one level up.
- **`XMsgCompleteIORequest` / `XamGetOverlappedResult`** are the two ends of the
  overlapped that the scheduled callback completes. The title pre-arms it itself
  (`[r29+0] = 997`, `[r29+8] = task block` in `sub_825D91E0`), so our side owes only
  the completion.
- **`XamShowDeviceSelectorUI` / `XamContentGetDeviceData` / `XamContentGetDeviceState`**
  are the storage device, under the same policy as the single local user and the
  single pad: exactly one device, always present, selected without UI.

**Recovering the message surface statically, instead of one run at a time.** A
dispatcher's real interface is the set of `(app, message)` pairs its caller can send,
and that set is in the image. Replaying the `r3`/`r4` assignments in the basic block
before each of the 18 `XMsgStartIORequest` / `XMsgInProcessCall` call sites recovers
all 25 of them in one pass:

    XGI  0xFB : 000B0006 0007 0008 0010 0011 0012 0013 0014 0015 001B 001C 001D 001E
                0021 0025 0026
    XLB  0xFC : 00000000 00058004 00058006 0005800E 00058020 00058023
    XMP  0xFA : 00070009 0007001B

Everything past `000B0008` is Live session, matchmaking, presence or media-player
work this runtime has no way to perform, so failing it is the honest answer rather
than a gap. Only `000B0006` (XamUserSetContext) is local bookkeeping, and its 24-byte
buffer layout is stated by `sub_825D7D20`, which fills it field by field before the
call: user index at +0, a zero doubleword at +8, context id at +16, value at +20.

That scan then got checked against a run, which is the part that makes it a
measurement: the boot produces **zero** "no handler" lines and exactly two handled
messages — contexts `0x0000` (presence) and `0x8001` (game mode), both for user 0.
The static surface and the dynamic one agree.

**Two more layouts off the guest rather than the SDK.** `XamTaskSchedule`'s second
argument is the *callback's context*, not a task-parameters struct — `sub_825D9358`'s
first real instruction is `lwz r30,12(r3)`, and `sub_825D91E0` passes `r4 = r31` with
the handle slot at `r31+24`, which A1 confirms as `(825D9358, 300E9000, 0, 300E9018)`.
And `XDEVICE_DATA` is 80 bytes with free space at +16: `sub_825D3648` zeroes `[r1+80]`
then memsets `r1+84` for 76 bytes, and the single field it reads back is the
doubleword at `[r1+96]`, which it compares against a required byte count.

**What this is worth, stated honestly: one of the nine ran.** Gate positions 82 and 83
(`KeResetEvent`, `XMsgStartIORequest`) now match hardware, and the router is exercised
and correct. The other eight — `XMsgInProcessCall`, `XMsgCompleteIORequest`,
`XamGetOverlappedResult`, `XamTaskSchedule`, `XamTaskCloseHandle`,
`XamContentGetDeviceData`, `XamContentGetDeviceState`, `XamShowDeviceSelectorUI` — are
**implemented but never reached**. Their layouts are all guest-derived and none of
them is a guess, but gotcha 30 applies without mercy: code that has never run has not
been shown capable of running. They are a prediction, not a result.

**And the reason they did not run is the useful finding.** The gate's next position
after `XMsgStartIORequest` is *not* the storage block. Hardware's 84 is `MmMapIoSpace`,
and A1 shows exactly where it comes from:

    d> F800010C MmGetPhysicalAddress(FFCA9000)
    d> F800010C MmMapIoSpace(00000002, 1FCAA000, 00000040, 00000404)
    A> F800010C XmaContext: reset context 0

That is the **XMA audio driver** mapping its register window, on the audio thread. Our
`XMACreateContext` is still a generated stub, so the driver never gets that far.

> **RETRACTED, same session.** The sentence that stood here — "everything at positions
> 85-92 is downstream of it" — was wrong, and wrong in a way worth keeping on the page
> because the gate invites it. A first-occurrence gate flattens a multi-threaded
> timeline into one sequence, so two positions can be adjacent in it while being
> causally unrelated. Checking the log lines and thread ids takes one grep and says so
> plainly:
>
>     84  MmMapIoSpace              A1 line  54,145   thread F800010C  (audio)
>     85  XamShowDeviceSelectorUI   A1 line 111,694   thread F8000008  (frontend)
>     92  XamContentGetDeviceData   A1 line 112,084   thread F8000008
>
> **57,500 log lines apart, on different threads.** XMA gates position 84 and nothing
> else. **Borne out by finding 36**: implementing the audio surface opened position
> 84 exactly, and the boot then stopped at 85 with the whole storage block already
> implemented and still unreached — which is what "not a domino" predicts. The storage block is gated on the *frontend thread* getting much further, and
> our run is currently loading `frontend/mainmenu.big` and `mainmenu.tex` — close to
> where A1 is, but A1 was driven by an operator holding a controller and our pad
> reports neutral because there is no host input source yet (finding 32). Positions
> 85-92 may well need input before they need any import.
>
> **Adjacency in a first-occurrence gate is not causation. Check the thread id and the
> line number before calling anything a domino.**

`XMACreateContext` still needs care rather than speed when its turn comes. It takes an
out-pointer and its caller tests the result with a signed compare, so a stub returning
a positive value would read as success — and CLAUDE.md gotcha 5 exists *because* Fable
2 lost weeks to exactly this import faking success.

### Finding 35 — a pseudo-handle is a constant, not an address

The A5 gate had `RtlNtStatusToDosError` at position 19 where hardware has it at 84 —
65 positions early, and the displacement pushed `NtWaitForSingleObjectEx` and
`KeWaitForSingleObject` late enough to account for two of the three remaining mismatch
windows. One import, three windows.

**Found in one run, with an instrument that already existed.**
`CZ_KCALL_WHO=RtlNtStatusToDosError` printed the guest stack at the first call:

    lr=825DA1F8  r3=C0000008        <- STATUS_INVALID_HANDLE
    #1 82821E80  #2 82789E88  #3 82496E78  #4 825D7488

`sub_825DA1E8` is `RtlSetLastNTError` — it converts the status and stores it in the
TEB at `[[r13+256]+352]`. Its caller `sub_82821E50` is `DuplicateHandle`:

    NtDuplicateObject(sourceHandle, targetOut, options)
    if (r3 < 0) { RtlSetLastNTError(r3); return FALSE; }
    return TRUE;

and the capture says what it was asked to duplicate:

    NtDuplicateObject(FFFFFFFE, 82AC3FE8(00000000), ...)

**`0xFFFFFFFE` is not a handle.** `GetCurrentThread()` on Win32 and the Xbox 360 kernel
returns a *pseudo-handle* — a reserved constant meaning "whoever is asking" — and code
passes it straight to `DuplicateHandle` or `ObReferenceObjectByHandle` to turn it into
something real. Our handle scheme is "a handle IS the object's guest address, with bit
31 set", which makes the two indistinguishable by construction: `0xFFFFFFFE` has bit 31
set, so it passed `IsKernelObject()` and was then rejected by the liveness check as a
dead handle.

The scheme already knew about the *other* reserved constant — `GUEST_INVALID_HANDLE_VALUE`
is `0xFFFFFFFF` and `IsKernelObject()` excludes it explicitly. It knew about -1 and not
about -2, which is the whole bug.

**The backtrace named the smaller half.** `NtDuplicateObject(FFFFFFFE)` appears twice
across A1 and A5. `ObReferenceObjectByHandle(FFFFFFFE)` appears **eleven** times, and
our implementation of that one is the identity — "a handle IS the object's address, so
referencing it is a no-op" — so it was handing the guest `0xFFFFFFFE` *as an object
pointer*, to be passed on to `Ke*` functions. Fixing only the site the crash-free
backtrace pointed at would have left the more frequent and more dangerous one wrong,
and silent. Grepping the captures for the constant, rather than for the import, is what
found it.

**The failure was silent by design.** Nothing crashed. `DuplicateHandle` returned FALSE
and the title took an error-reporting path — `sub_82789CF8` falls through to a `bctrl`
with a message id — that hardware never enters. A boot that "works" was calling its own
error logger before it finished starting up.

**The fix is a real per-thread object,** `GuestThreadSelf`, minted on first use and
cached in a `thread_local` with an extra reference so a guest closing its duplicate
cannot dangle it. It deliberately does *not* reuse `GuestThreadHandle`: that type owns
the `std::thread` it spawned and answers `Wait()` by joining it, and a thread asking
about *itself* is not the thread that spawned it — the main guest thread was never
spawned by us at all. `GuestThreadSelf` carries an exit flag that the thread bootstrap
sets when the guest entry point returns, and polls it.

**Result — the A5 boot now has one real difference left.**

    before: 3 mismatch windows, 0 permutation, 3 real
    after:  3 mismatch windows, 2 permutation, 1 real

The two windows that became permutations were never independent; they were the wake of
this one displacement. The survivor is `XAudioSubmitRenderDriverFrame`, absent because
there is no audio backend. Our `RtlNtStatusToDosError` now sits at position 83 against
hardware's 84.

**Transferable.** Any runtime that encodes handles as addresses inherits this the
moment the guest uses a reserved constant. The platform defines two — `-1`
(current process, and also `INVALID_HANDLE_VALUE`) and `-2` (current thread) — and a
scheme that special-cases one of them looks correct right up until the title asks for
the other.

### Finding 36 — the audio driver calls back through a pointer, and the whole subsystem hinged on one load

`runtime/kernel/audio.cpp`. Seven imports, one gate position, and one ABI detail that
is invisible from every direction except the guest's own code.

**Why it was the last real divergence.** `XAudioSubmitRenderDriverFrame` is not a call
the title makes. It is made from a callback that the *audio driver* invokes, so with a
stubbed `XAudioRegisterRenderDriverClient` there was nothing to invoke it and the whole
audio path was absent without a single error line. A stub that fails honestly still
deletes everything downstream of it; that is the cost of honesty, and it is only
visible in a gate.

**The layout came from the guest, because no capture can supply it.** Xenia prints an
import's pointer arguments as they were *before* the call, so it never shows what an
audio import wrote (gotcha 60). Everything below is read out of the image:

| what | where it is stated |
|---|---|
| frame = 256 samples x 6 channels x f32, planar, 1024-byte channel stride | the de-interleave loop in `sub_828867E8` (`li r8,256` / `li r9,6` / `stfsu f0,1024(r7)`) |
| speaker config: only bit 31 is read, and must be CLEAR | `rlwinm r11,r11,0,0,0` in `sub_82886B70`; anything else contradicts the title's own 4-byte-per-sample stride. No other bit is read anywhere in the image, so the rest of the value we return is not evidence about the console's encoding |
| driver handle is opaque, but must be non-zero | `sub_828868D8` skips the unregister entirely on a zero handle |
| XMA context = 64 bytes, in one array | `(MmGetPhysicalAddress(ctx) - base) >> 6` in `sub_8285EE58`, and independently `MmMapIoSpace(2, phys, 64, 0x404)` |
| the XMA context-array base lives in an MMIO register | `sub_8285EDF8`, whose entire body is `lwbrx r11,0,r11` from `0x7FEA1800` |

The XMA decoder's register file is at **`0x7FEA0000`**, it is **little-endian** (both
accesses are `lwbrx`/`stwbrx`), `+0x1800` holds the context array's physical address,
and `+0x1A80` is a one-bit-per-context kick bitmap indexed `>> 5`. That is console
knowledge, not title knowledge — it should transfer to Case West and to any 360 port
whose title drives XMA directly instead of through the kernel.

**The finding proper: the callback receives a POINTER to the context, not the context.**

The registered callback `sub_828869B0` is two instructions:

```
lwz r3,0(r3)
b   0x828867E8
```

It dereferences its argument before doing anything. Passing the registered context
straight through in `r3` — the obvious reading, and what a naive driver does — hands
the real body a pointer one indirection too shallow. The body then reads its wait
objects, its sample buffer and its driver handle out of whatever happens to sit there.

What proves the indirection is deliberate rather than a quirk is the constructor,
`sub_828869B8`:

```
stw r9,0(r3)     ; primary vptr   0x820D241C
stw r8,4(r3)     ; secondary vptr 0x820D23F0   <- +12 of it is sub_82886B70
stw r11,32(r3)   ; driver handle  ) all zeroed at construction, all read
stw r11,44(r3)   ; wait object    ) back by sub_828867E8 at the SAME offsets
stw r11,48(r3)   ; wait object    )
std r11,56(r3)   ; frame counter  )
```

`sub_82886B70` (the registration) is slot 3 of the **second** vtable, so its `this` is
`obj+4` and the `addi r9,r31,-4` that builds the callback context is recovering
`obj+0`. Meanwhile the callback body measures every field from `obj+0`, matching the
constructor field for field. So the body wants `obj`, the registration supplies `obj`,
and the thunk in between performs exactly one load — therefore the driver must pass a
pointer *to* the registered context. We keep a one-dword guest cell for it, which is
also required for a second reason: the title builds its `{callback, context}` pair on
its own stack (`addi r3,r1,88`) and returns immediately, so a driver that did not copy
the pair would be reading a dead frame.

Getting this wrong is not subtle in its consequences and is entirely silent in its
cause. Our first run handed the body a pointer to a table of audio constants in
`.rdata`; every wait object came back NULL, and the process died with a SIGSEGV inside
our own kernel.

**Three sub-findings, each general.**

*A guest-supplied pointer must never be dereferenced without a check, however
impossible the null looks.* `WaitDispatcher` read `header->Type` unguarded, so **any**
guest passing a null dispatcher object to **any** `Ke`-level wait took the host down at
address 0 — a fault the crash reporter correctly labels "outside the 4 GB guest space,
a host-side bug", which is a true statement that names our kernel rather than the guest
that provoked it. On hardware this would bugcheck, so there is no faithful behaviour to
copy; what there is instead is the rule. `KeWaitForMultipleObjects` now validates the
whole array up front and fails, rather than polling a null forever — and because this
call site is an infinite wait, polling would have wedged the thread for the life of the
process.

*A driver's callback is a frame-clock event, so the pump must sleep before the first
one, not after it.* The title registers the client from one thread while the mixer
thread is still constructing the object, and A5 shows hardware taking the same shape —
709 log lines separate the registration from the first
`XAudioSubmitRenderDriverFrame`. But the delay is fidelity, **not** the fix: a race lost
by 5 ms is still a race. What makes it correct regardless of timing is that an early
callback now returns without mixing and the next frame retries.

*An allocation the runtime owns must get out of the guest's way.* `Audio_Init` first
took its context array from the bottom of the physical arena, which moved the title's
own 447 MB reservation from `A0000000` to `A0005000` — a change to an unrelated
subsystem bought for nothing (gotcha 9). A1 shows Xenia doing the opposite: the title
gets physical `0x03D93000`, and the XMA context array sits at `0x1FCAA000`, one page
*past* the end of that reservation. `GuestHeap::AllocPhysical` grew a `topDown` flag,
and the title's addresses went back to being byte-identical to the baseline's.

**Measured.**

```
--xenia A5 --include-high-frequency :  3 windows / 1 real  ->  2 windows / 0 REAL
                                       tracks to position 119, A5's last
                                       "SET MATCH: every mismatch is a permutation. Exit 0."
--xenia A1                          :  exact 84-deep prefix of Xenia's 93
                                       position 84 = MmMapIoSpace -- the domino is open
```

and the XMA path is confirmed against the capture rather than merely reached:

```
ours   MmMapIoSpace(bus=2, phys=1FFEB000, 64 bytes, protect=404)
A1     MmMapIoSpace(00000002, 1FCAA000, 00000040, 00000404)
```

identical in every field but the address, which differs only because we place the array
at the top of the arena and Xenia places it just past the title's reservation. One
context created, exactly as in A5.

**What this is not.** There is no audio output and no XMA decoding. Frames are counted
and dropped, which is a real null sink rather than a fake success — the contract of
`XAudioSubmitRenderDriverFrame` is "the driver has taken this buffer". `CZ_AUDIO_TRACE=1`
prints a peak amplitude every 512th frame precisely because "the pump is running" and
"the game is producing audio" are different claims and a frame count cannot separate
them; through the boot the peak is 0.0000, i.e. the mixer is submitting silence.

**What ran, and what did not** (gotcha 67 — implemented is a prediction, not a
result). Five of the seven executed in a 90 s boot: `XAudioGetSpeakerConfig`,
`XAudioRegisterRenderDriverClient`, `XAudioGetVoiceCategoryVolume`,
`XAudioSubmitRenderDriverFrame` (thousands of times) and `XMACreateContext` (once,
exactly as in A5). The two that did not are both teardown —
`XAudioUnregisterRenderDriverClient` and `XMAReleaseContext` — because the boot never
shuts the audio device or the stream table down. Their argument handling is derived
from `sub_828868D8` and `sub_8285EF68` and is unproven.

**Stability, with its own control arm.** The pump is a new thread running guest code,
which is exactly the shape of thing that has destabilised this runtime before, so it
was A/B'd against `CZ_NO_AUDIO_PUMP=1` — the same binary, registering the client but
never invoking the callback:

```
ARM pump   : 0 crashes in 8 runs at 25 s
ARM nopump : 0 crashes in 8 runs at 25 s
```

Read that as "no measurable difference", **not** as "the pump is safe". Eight runs
cannot see a 1-in-20 fault, and the known surviving crash is around that rate
(gotchas 50-51). Both arms ran in one session on one binary; the first attempt was
discarded because a rebuild landed mid-run and split the arms across two builds.

**Timing note worth recording, because it nearly produced a false negative — twice.**
At 30 s the gate stopped at position 114 and the XMA path looked unreached. It is not
blocked, it is *slow*: at 90 s the same binary reaches 119. A duration is part of a
gate's configuration, and "not reached" and "not reachable" are not the same
measurement.

The second near-miss is the one worth the ink. The first draft of this finding said
"at 90 s the same binary reaches 119" on the strength of **one run** — and the very
next 90 s run did not reach it. Gotchas 50-51 are about crashes, but they are really
about single runs of anything: *how far a multi-threaded boot gets in a fixed wall
time is a distribution, not a fact.* Measured properly:

```
at  90 s : 3 of 3 runs reached MmMapIoSpace (A1 position 84) -- plus 1 earlier run that did not
at 150 s : 2 of 3
```

so 5 of 7 across this session. Usual, not guaranteed. Gate the XMA positions on a
long run and expect to repeat it.

One of the 150 s runs segfaulted, and it is the **known** fault, not a new one: guest
thread `00000F2C`, `lr=8284B708`, `ctr=00000000`, backtrace
`8284B708 <- 8284B9AC <- 82829BEC` — identical to the report in section 6's item 2,
which is another instance of gotcha 56 (these crashes repeat byte for byte; only
whether the boot reaches the site varies). Nothing in this finding's changes is
implicated.

### Finding 37 — the frontend IS waiting for input, and proving it needed an arm that can lie

The question this answers is the one a gate cannot: our boot settles with the
renderer running, the file count flat and the kernel-call profile steady, and **a
title screen that is finished and waiting for a human looks exactly like one that is
stuck**. Same frame rate, same file count, same polls. The only thing that
distinguishes them is whether input makes it move — so the only way to find out is to
supply some.

**First, the premise had drifted and had to be re-measured.** The standing note said
"our run reaches `frontend/mainmenu.tex` and then has nothing to press START with".
That was true when written and is no longer the interesting part: the runtime has
since moved on, and a healthy run now opens **64 files**, ending with
`data\models\environment\prologue_menu\prologue_z01.big` — the title screen's 3D
scene, not its texture. A1 loads the same thing in the same order right after
`mainmenu.tex`. And the renderer is not idling behind it:

```
120 s run:  120,663,964 PM4 packets   4,118 XE_SWAP frames   8,160,035 draws
            ~34 fps, ~1,982 draws/frame
```

against A1's title-screen era of ~2,540 draws/frame (finding 10) — different
instruments, so indicative rather than identical, but the same order of magnitude and
the same shape. **We are rendering the title screen**, not stalled before it. Gotcha
13 says re-read a capture request against the current ledger before believing it; this
is the same rule turned on our own notes.

**The arm.** `CZ_FAKE_START_MS=N` in `XamInputGetState_x`: a synthetic START press
every N ms, held 150 ms, with the packet number incremented on each transition
(XInput's contract is that the packet number changes only when the state does, so a
constant one hands the guest a press it is entitled to ignore).

It is off by default and it announces itself on every press, and that loudness is the
point rather than politeness. **A fake button press manufactures progress.** A run
that quietly had this on would show the boot advancing past the title screen and
invite the conclusion that some import we had just written unblocked it. Nothing may
progress on the strength of a run whose input was invented — so it is named for what
it is, it logs `SYNTHETIC INPUT IS ON ... do not gate on it`, and gate runs must not
use it.

**The measurement.** Three runs per arm, same binary, 120 s:

| run | files | presses | reached pos. 85 | outcome |
|---|---|---|---|---|
| noinput_1 | 57 | 0 | no | stalled during load |
| noinput_2 | **64** | 0 | **no** | title screen, no advance |
| noinput_3 | 47 | 0 | no | stalled during load |
| input_1 | **64** | 5 | **YES** | **advanced** |
| input_2 | 57 | 0 | no | stalled during load |
| input_3 | **64** | 5 | **YES** | **advanced** |

Every run that reached the title screen **and** received a press advanced; the one
that reached it without a press did not. The causality is tight enough to read
directly out of the log — `XamShowDeviceSelectorUI` appears **five lines after** the
first `synthetic START DOWN` and before its release:

```
[kernel] CZ_FAKE_START_MS: synthetic START DOWN at 20s (packet 2)
[kcall] XamShowDeviceSelectorUI
[kernel] CZ_FAKE_START_MS: synthetic START up at 20s (packet 3)
```

and the A1 gate moves from an 84-deep prefix to an **85-deep** one, position 85 being
`XamShowDeviceSelectorUI` exactly.

Note `input_2` in the table: it got **zero** presses because it stalled before the
frontend began polling. That is the arm being honest — it presses only once the title
asks for pad state — and it is why the pairing above is 2-of-2 rather than 3-of-3.

**Where it stops next, and why that is not a mystery.** After the device selector the
gate wants position 86, `XamGetPrivateEnumStructureFromHandle`, and then
`XamContentCreateEnumerator` / `XamEnumerate` / `XamContentCreateEx` /
`XamContentClose`. All five are still generated stubs, deliberately: they are the
phase 2 save-data layer, explicitly carved out of finding 34. So the frontend is now
blocked on a gap we chose, not one we have to find.

**A separate defect found on the way: an intermittent load stall.** Three of the six
A/B runs never reached the title screen at all, stopping at 47 files
(`frontend\mainmenu.tex`) or 57 (`cinematics\cinematics.big`). In that state the wait
trace finds the **main thread parked in the renderer's frame fence**:

```
[wait] tid=00000F00 (entry=00000000) handle=BBF171C0 lr=825DA5F0 stuck 110s EVENT
  #1 827CC6D8   sub_827CC6A8  -- WaitForSingleObjectEx([obj+2480], INFINITE, alertable)
  #2 827CC79C   sub_827CC770  -- frame: wait fence, do work, NtSetEvent([obj+2476])
  #6 825DA0C0   sub_825D9F28  -- main()
[csspin] r13=88304DA0 wants cs ownedBy r13=88004D60 (the main thread) rec=1
[csspin] r13=88314DA0 wants cs ownedBy r13=88004D60 rec=1
```

i.e. the main thread blocks on the render fence **while holding two critical sections
two other threads are spinning on**. The other side of that fence is
`sub_827D3898`, which is the Draw Thread body (its `NtSetEvent([r31+2480])` is the
signal, and the thread entry `0x827D3B40` sits at its end — A1 and our own
`ExCreateThread` log agree). Not chased further here; it is a distinct fault from the
question this finding answers, and it is now its own task.

Rate, and the caveat that goes with it: **4 of 8** long runs on the current binary
stalled, against **1 of 6** on the previous one. Those two numbers are not consistent
with each other, which is gotcha 51 exactly — a rate measured once is a fact about
that afternoon — so treat "about a third to a half" as the current honest range and
re-establish it before and after any change aimed at it.

### Finding 38 — the load stall: the Draw Thread is waiting for a fence packet we never executed

The symptom, from finding 37: a third to a half of long runs never reach the title
screen. They stop at 47 or 57 files with the main thread parked in the renderer's
frame fence, waiting for the Draw Thread to signal a frame it never signals.

This finding traces that end to end and stops one step short of a fix. What it
establishes: **the Draw Thread is spinning in guest code on a ring-progress counter
that only the command stream advances, and our command processor stopped executing
the packets that advance it** — because its walk of the title's indirect command
buffers ends early, silently, on data it reads as a packet header. What it does not
yet establish is why those bytes are not the bytes hardware saw; a 24.5-million-packet
check against the B1 capture rules out the first three theories, including the one
this finding first proposed and had already written up as the answer.

**First, a retraction.** The task written from finding 37 recorded that the main
thread blocks on the fence *while holding two critical sections two other threads are
spinning on*, and read that as a possible deadlock cycle. Both halves are true and
they are unrelated. With `CZ_CS_TRACE=1` a **healthy** run shows the same two
`[csspin]` lines, on the same two sections, from early boot to the end of the run —
they are `DnsLookupThread` and `session shutdown thread`, each blocked at its first
`RtlEnterCriticalSection` on a section the main thread holds for the life of the
process. That is the title's own design; on console those two threads simply sleep.
Adjacency in a stall trace is not causation (gotcha 68), and the give-away was
available for free: the same lines are in the healthy runs.

#### The instrument that had to exist first

The wait trace covered `NtWaitForSingleObjectEx` and nothing else. The Draw Thread
waits in `WaitForMultipleObjectsEx` over four handles, so the single most important
wait in the title was the one wait our tracing could not see, and its silence read as
"that thread is fine". Three instruments came first:

- `WaitObject`, one traced-wait helper shared by the handle-level and
  dispatcher-header paths, plus a report from inside both wait-any polling loops.
- a `[kernel] guest thread ... ENDED` line, always on. A dead producer and a producer
  that has not got round to signalling look identical from the consumer's side.
- the host thread id per guest thread, and a PCR→thread-id map so `[csspin]` names
  both sides of a contention by entry point rather than by raw `r13`.

With those, a caught stall reads: main parked on the fence, six JobThreads parked in
their wait-any, the audio and file threads parked on semaphores — **and the Draw
Thread in none of them, and not ended.** A thread that is stuck without being in any
wait is running guest code, and nothing inside the runtime can say where, because it
is not calling us.

#### Outside the process

`gdb -p` on a caught stall, joined to the log by the host-tid line:

```
Thread 9 (LWP 2505924):
#0 __imp__sub_8283C6C8   ppc_recomp.173.cpp:34882
#1 __imp__sub_82845160   ppc_recomp.175.cpp:16621
#2 __imp__sub_82841F00
#3 __imp__sub_827D3898        <- the Draw Thread body
```

`sub_8283C6C8` is a spin-with-backoff predicate (eight `mr r31,r31` pause hints in a
`bdnz` loop, then a timeout check). `sub_82845160` is the loop that calls it, and it
names the whole protocol:

```
82845200  lwz  r11,0x2a90(r31)   ; r11 = the progress-word POINTER
82845204  lwz  r10,0x2a9c(r31)   ; W = what the driver has produced
8284520C  lwz  r11,0(r11)        ; R = what the GPU has consumed
82845210  subf r11,r11,r10
82845214  cmplw cr6,r9,r11       ; (W - target) >= (W - R)  ->  R has reached target
82845218  blt  cr6,0x828451f0    ; ...else spin again, forever
```

A gdb script that finds the thread whose frame 1 is `sub_82845160` and reads those
three numbers out of guest memory gives, on two separate catches:

```
RINGWAIT dev=40001D80 target=00000509  R=00000507  W=0000050D
RINGWAIT dev=40001D80 target=00000519  R=00000517  W=0000051D
```

Both times R is frozen exactly **two** short of the target. Meanwhile
`CZ_RING_TRACE=1` says our command processor is completely caught up:

```
ring: kickedWptr=00000F31 [wb+0]=00000507 | registered=BC7394BC [reg+0]=00000F31
      | cursor=3889 scratch=1BF39460 | mmio CP_RB_WPTR=00000F31
ring: pm4 packets=446726 frames(XE_SWAP)=321 draws=25102 interrupts=433   (frozen)
```

`cursor` = `kickedWptr` = the MMIO write pointer, and the packet count does not move.
There is nothing left to execute. The driver is waiting for a number only new packets
can produce, and it will not produce new packets until the number moves.

#### Two words, not one

That trace also shows something worth keeping: `[wb+0]`, the word the driver polls,
and `registered`, the slot `VdEnableRingBufferRPtrWriteBack` handed us, are
**different addresses** — 0xBC739480 and 0xBC7394BC. The image says why:

```
82846484  li   r3,0x60          ; a 96-byte block
82846488  bl   <alloc>
8284648C  stw  r3,0x2a90(r31)   ; the driver polls [block + 0]
...
82846500  lwz  r11,0x2a90(r31)
82846504  addi r11,r11,0x3c     ; and registers block + 0x3C with the kernel
8284651C  bl   VdEnableRingBufferRPtrWriteBack
```

So the read pointer we publish — correctly, into the slot we were given — is not the
word this wait reads. A `gdb` hardware watchpoint on the polled word in a **healthy**
run names its real writer in one hit:

```
Thread 16 hit Hardware watchpoint: *(g_memory.base + 0xBC739480)
#0 StoreGpu                gpu/pm4.cpp:239
#1 ExecutePacket           gpu/pm4.cpp:396      <- the EVENT_WRITE family
#2 ExecuteLinear           gpu/pm4.cpp:597      <- inside an INDIRECT_BUFFER
#3 ExecutePacket ... #4 Pm4_Execute ... #5 GraphicsInterruptPump
```

The driver's progress counter is written by **its own fence packets, in its own
command stream**. Which turns the question into: why did we stop executing them?

#### The truncation

`ExecuteLinear` stops when a packet claims more dwords than the buffer holds. That is
the right thing to do and it was doing it **silently**. Made loud, a 60 s run reports:

```
[pm4] indirect buffer TRUNCATED at dword 7227 of 7244 (va=BC17D580, header=00E48000)
[pm4] indirect buffer TRUNCATED at dword 73 of 135 (va=BC2BFA60, header=22000000)
[pm4] indirect buffer TRUNCATED at dword 96 of 151 (va=BC27EC40, header=3F800000)
```

`3F800000` is the float 1.0. We are parsing *data* as a packet header, so the walk had
already desynced; the reported position is where the invented length finally ran off
the end, not where the mistake was. A trail of the last eight packets and a raw dump
of the untrusted tail (both added here) show what is lost: the tail of one of these
buffers decodes cleanly as `REG_RMW`, a register write, `INVALIDATE_STATE`, and two
`EVENT_WRITE_SHD` — the last of which writes `0000051D` to physical `1C739482`, which
is the polled word with endian code 2. **The packet we drop is the fence the Draw
Thread is waiting for.** One dropped packet, one thread waiting for the rest of the
process's life.

That closes the mechanism. What remains is why the walk desyncs.

#### A wrong answer, and the oracle that caught it

The first answer looked excellent: a zero dword is padding, and we read it as a
two-dword type-0 packet ("write one register at index 0", swallowing the dword after
it as data), so every odd-length run of padding shifts the walk by one. Dumping
offending buffers (`CZ_PM4_DUMP_TRUNCATED`) and re-walking them offline under both
rules seemed to confirm it — a 135-dword buffer went from "stops at dword 73 on
`22000000`" to "ends exactly at 135, on the fence packet".

It is wrong, and the thing that says so is a check that should have existed from the
start. Xenia's `.xtr` records a `PacketStart {base_ptr, count}` for **every packet it
executed**, and `count` is that packet's true length in dwords — the boundary real
hardware used, on this title, inside indirect buffers included. `tools/pm4_packet_lengths.py`
compares that against the rule `gpu/pm4.cpp` uses:

```
  packets checked      : 24,527,474
  lengths agreeing     : 24,527,473
  lengths DISAGREEING  : 1
      type 0 header 00000000: hardware used 2, we use 1
```

Our packet-length arithmetic was **already correct on 24.5 million real packets**, and
the single disagreement is the zero rule I had just changed — recorded by hardware as
two dwords, i.e. the behaviour I had removed. B1 contains exactly one zero-header
packet in 24.5 M, which is the other half of the story: a correctly aligned walk of
this title's streams essentially never meets one. The zeros we trip over are *data*,
seen through a walk that is already lost. The change was reverted; it could only ever
have altered where an already-desynced walk came to rest.

With it reverted the same command prints what it should, and that is the state the
tool is committed in — run it after any change to the packet decode:

```
  packets checked      : 24,527,474
  lengths agreeing     : 24,527,474
  lengths DISAGREEING  : 0

OK: every packet's recorded length matches the rule in gpu/pm4.cpp.
```

The same oracle also kills two follow-up theories in one pass. Hardware never executes
a packet with header `00E48000` (0 occurrences in 24.5 M), so that dword is data, not a
packet we mis-size. And hardware's own indirect buffers run to 65,522 dwords and are
executed whole, so "the big ones are a different path" is not it either.

Measured rather than asserted, per gotcha 50 — `CZ_PM4_ZERO_IS_PACKET=1` gave a
same-binary control arm, 10 runs each at 120 s:

| arm | stalls |
|---|---|
| zero read as a 1-dword no-op (the "fix") | **3 of 10** |
| zero read as a packet (the shipped rule) | **4 of 10** |

against a **2 of 8** baseline on the committed binary. No effect, which is what the
capture predicts for a rule that can only fire once the walk is already lost — and
worth noticing that the arm with the "fix" is the one that stalled less by a margin
that means nothing. Had this been run as a single pair of runs instead of ten, it
would have produced a decisive-looking answer in either direction (gotcha 50).

#### A false alarm, recorded because it nearly cost a good change

Partway through, the A1 gate started reporting `DIVERGENCE at position 71` — a
permutation of `XamUserCheckPrivilege` against the two `Xex` calls at 71-73 — in about
half of the runs that reached the title screen, where the committed binary had given a
clean 84-deep prefix in 6 of 6. That reads as an obvious regression from this
session's changes, and the two suspects were both new: firing APCs at the multi-object
waits, and a finite timeout that can now actually expire there.

Both were exonerated by measurement rather than by reading. The finite-timeout path
was instrumented and **never fires** — no caller in this boot passes one — so it
cannot reorder anything. And the decisive control was the obvious one that is easy to
skip: `git stash`, rebuild the **committed** binary, and run it again *today*. It
produces the same permutation.

Then, properly: both binaries built side by side, runs alternated so neither arm owns
a stretch of wall-clock, six each.

| binary | position-71 permutations |
|---|---|
| committed (`cz_base`) | 1 of 6 |
| this session's | **0 of 6** |

Positions 71-73 were always scheduling-sensitive; the 6-of-6-clean sample was smaller
than it looked, and the 6-of-9-permuted sample that raised the alarm was equally an
afternoon. Neither number was about the code.

Gotcha 51 says a rate measured once is a fact about that afternoon. The corollary this
adds: **the control for "did my change do this" is the old binary run now, not the old
binary's remembered numbers.** Reverting on the remembered ones would have thrown away
a correct change to fix a defect that did not exist.

(The APC drain is still off by default, but for the honest reason rather than that
one: it was written to chase this stall, the stall turned out to be elsewhere, and
nothing has yet shown it is needed. `CZ_MULTIWAIT_APC=1` enables it.)

#### What is actually left

Our lengths match hardware; our start addresses come straight from the packet; the
buffers still desync, at a position that varies from buffer to buffer (dword 73 of
135, 7227 of 7244, 10056 of 10106). That leaves one class of explanation: **the bytes
we walk are not the bytes hardware walked.** The leading suspect is timing — our
command processor runs on the 16 ms vblank tick rather than continuously, so it reads
each indirect buffer up to a frame after the driver submitted it.

Stated as a suspicion, not as a finding, because the evidence for it is weak and it
would be easy to overstate. Two different buffers carry the identical 7-dword run
`00022204 00010000 00010000 00000300 00002312 00001844 00E48000` immediately before
their closing packets, which reads as "stale bytes from a previous frame" and reads
just as well as "a fixed preamble the driver emits before every epilogue" — the second
being the likelier of the two. What is certain is only that our walk arrives at those
dwords misaligned.

The next experiment is written down rather than run: snapshot each indirect buffer
before walking it, walk as usual, then compare the snapshot with live memory
afterwards. If they differ, the guest is writing the buffer while we read it and the
fix is about *when* we consume the ring, not how we parse it. That is a much larger
change than a parser fix — it is the difference between a command processor driven by
the vblank and one driven by the write pointer — which is why it is a separate task
rather than a patch at the end of this finding.

> **Resolved by finding 39, and the timing suspicion above is retracted.** The bytes
> we walked were indeed not the bytes hardware walked, and nothing was overwriting
> them: they were the previous frame's packets, in 52 dwords our own `VdSwap` never
> filled. The snapshot experiment was run, found zero concurrent writes in 84,808
> buffers, and is what let the timing theory be dropped rather than pursued.


### Finding 39 — the stall was our own kernel: `VdSwap` left 52 dwords of its reservation unwritten

Finding 38 traced the hang to a single dropped fence packet and left one question:
why does our walk of the title's indirect buffers desync when our arithmetic matches
hardware's own boundaries on 24.5 million packets? The answer is that the arithmetic
was never the problem. **The command processor was being handed the previous frame's
packets to walk, by us.**

`VdSwap` is the kernel export that writes the frame-swap packet into a block of
command buffer the guest hands it. Ours wrote 12 dwords — a 7-dword front-buffer
fetch constant and the 5-dword `XE_SWAP` — and returned. The caller reserves **64**
dwords and advances its write pointer by the whole reservation whether or not the
kernel fills it, so the 52 dwords we left alone were submitted to the command
processor exactly as if the kernel had put them there. Command buffers are recycled,
so what was actually in them was the last frame's packets: real headers at wrong
offsets. The parser read one, invented a length from it, ran past the end of the
buffer and stopped — dropping every packet after it, including the ring-progress
fence that is the last packet in these buffers.

So the defect was never in `gpu/pm4.cpp` at all. It was one missing loop in
`gpu/vd.cpp`, and it presented as a parser bug for two sessions.

#### Ruling out the theory that had the most going for it

Finding 38's leading suspect was timing: our command processor consumes the ring on
the 16 ms vblank tick, so it reads each indirect buffer up to a frame after the driver
submitted it, and a driver recycling command memory would be writing under us. That is
a real hazard, it explains the symptom, and acting on it means rewriting the command
processor to be driven by the write pointer instead of the clock.

`CZ_PM4_IB_VERIFY=1` settles it for the cost of a `memcpy`: snapshot every indirect
buffer before walking it, walk it exactly as usual, compare afterwards.

```
ring: indirect buffers truncated=2172 | verify clean=84808 dirty=0
```

**84,808 buffers walked, not one modified**, in a run that truncated 2,172 times. The
instrument is deliberately slow — it doubles the reads over every buffer — and the
asymmetry matters for how that reads: slowing the parser makes a concurrent write more
likely, so "clean" under a handicap is the strong direction of the result. The timing
theory is dead, and no command-processor rewrite is needed.

#### The oracle that should have existed first, again

`tools/pm4_packet_lengths.py` proves each packet's length is right. That is not the
same as proving a *walk* is right: it says nothing about the address a walk starts at
or about whether the packets tile. `tools/pm4_indirect_walks.py` closes that gap, and
it is worth stating what makes it possible. Xenia walks *into* each indirect buffer
rather than dereferencing guest memory, so every packet inside one is recorded with
its own `base_ptr` — the address hardware read it from. Chaining our own length rule
through those addresses replays our cursor against every boundary hardware landed on,
from the buffer's first dword:

```
  indirect buffers     : 28,727
  dwords CONSUMED      : min 10, max 65522, mean 5623
  buffers DISAGREEING  : 1        <- an artifact, below

OK: every indirect buffer's address and size match the packet, and hardware's
    own packets tile it exactly under our length rule.
```

With both halves of the parser cleared against ground truth, the only thing left was
the input — which is what turned the hunt around. The general form is finding 38's
gotcha 80 applied to the layer above: an oracle for your arithmetic does not clear your
*inputs*, and a clean parser gate is not a clean command processor.

#### Reading the desync backwards: what a wrong dword is worth

The truncation reports name a position, and it is data by then (gotcha 85). Six
dumped buffers all reported different stopping points — dword 73 of 135, 7227 of 7244,
10036 of 10106 — and none of them was where the mistake happened.

What located it was building a dictionary from the capture: **225 distinct packet
headers over 24,527,474 packets**, a remarkably tight vocabulary. Walking each dumped
buffer and flagging the first header that is not in it puts the boundary in one place:

| buffer | last header hardware recognises | breaks at |
|---|---|---|
| 7244 dwords | `C0036400` XE_SWAP @7169 | 7174 = swap + 5 |
| 7342 dwords | `C0036400` XE_SWAP @7267 | shortly after |
| 135 dwords | `C0036400` XE_SWAP @60 | 69 |
| 10106 dwords ×2 | `C0036400` XE_SWAP @10031 | 10036 = swap + 5 |

Every one of them, the swap packet. And the swap packet is the one packet in the
stream *we* write.

#### What hardware puts there

```
--- XE_SWAP in IB 03570100 ---
    @03572C90 len 5: C0036400 53574150 04BDC000 00000500 000002D0
    @03572CA4 len 1: 80000000
    @03572CA8 len 1: 80000000
    ... 52 of them ...
    @035B04A4 len 4: C0025800 80000003 03D7104C DEADBEEF     <- the epilogue
    @035B04B4 len 2: 00001844 04BDC000
    @035B04BC len 4: C0022100 00001841 FFFFF8FF 00000000
```

`0x80000000` is a type-2 PM4 packet: a one-dword no-op, the only encoding that lets a
parser cross an arbitrary run of dwords without interpreting any of them. Across B1's
43 indirect buffers containing a swap, the count of them immediately following it is
**52, every time, with no exceptions** — 12 dwords of content plus 52 of padding.

The title already knows this idiom: it prefills its scaler command buffer with exactly
this value via `RtlFillMemoryUlong`, which is the observation that made our
`VdInitializeScalerCommandBuffer` stub safe to leave empty. The same reasoning was
available for `VdSwap` and was not applied to it.

One precision about what that capture is evidence *of*, because it is easy to overstate
and this port's convention of saying "hardware" for "what B1 shows" hides it here.
`VdSwap` is an HLE kernel export in Xenia too, so those 52 no-ops were written by
Xenia's own kernel, not by the guest and not by silicon. B1 is therefore a reference
implementation agreeing with us about the value and the count — good corroboration, not
ground truth. The ground truth is the next section: it comes from the guest.

#### The number is in the image, not only in the capture

The size did not have to be inferred from the capture at all. The call site says it:

```
    bl      VdSwap
    addi    r11,r29,256      ; advance by 256 bytes = 64 dwords
    stw     r11,48(r31)      ; ...regardless of r3, which is never read
```

64 dwords reserved, 12 written by us, 52 left over — matching the capture's 52 exactly,
from a completely independent witness, and the one that is not another implementation's
opinion. Two witnesses, neither a guess about the SDK, which is the standard finding 30
set for this kind of constant.

It also settles the part the capture cannot: that the guest *submits* the tail whether
or not anyone fills it. The write pointer moves by 256 bytes unconditionally, so those
52 dwords reach the command processor in every implementation. Xenia's fills them; ours
did not.

It also means the return value was never load-bearing: `r3` is dead at the call site.
Ours returned 12 and could have returned anything.

#### Why the zero-header story was a red herring — and a second retraction

Finding 38 recorded that hardware consumed its single zero-header packet as *two*
dwords, and used that to reject reading a zero dword as a one-dword no-op. The
observation was real; the classification was wrong.

That packet is an `INDIRECT_BUFFER` whose header the trace records as zero. It carries
every marker of one: `PacketStart.count` of 2 (the short recording every 0x3F gets),
a command-buffer address in word[1], and an `IndirectBufferStart` as the very next
trace command with `base_ptr` equal to that word — the mechanism measured across all
28,726 buffers. It sits at guest address `03D71FFC`, the last dword of a 4 KB block;
why the header reads as zero there is not established and does not need to be.

So **B1 contains no genuine zero-header packet at all**, and the capture is silent on
what hardware does with a zero dword. It supports neither reading.

That silence is the tell, in hindsight. Hardware's streams contain no zero dwords
because hardware's tails are full of `80000000`; ours contained zeros only where the
buffer memory happened to be fresh, and the previous frame's packets everywhere else.
Both were the same hole. `tools/pm4_packet_lengths.py` now classifies that packet by
the mechanism rather than by its header bits, so its clean result stops being an
artifact that agrees by coincidence.

`CZ_PM4_ZERO_IS_NOP` stays as an arm. It is no longer interesting, and that is the
useful thing to record about it: the "fix" it represents removed *some* desyncs (two
of six dumped buffers walk to completion under it) without touching the cause, which
is exactly the shape of a change that measures as a partial improvement and is wrong.

#### Measured

Same binary, both arms, runs alternated (gotcha 86). `CZ_NO_SWAP_PAD=1` restores the
pre-finding-39 behaviour.

Six runs a side at 120 s, `trunc` = indirect buffers whose walk ended early,
`files` = the last `NtCreateFile` ordinal (63 = reached the title screen, 46 = stalled):

| run | tail filled (the fix) | `CZ_NO_SWAP_PAD=1` (the defect) |
|---|---|---|
| 1 | trunc 0, files 63 | trunc 3, **files 46** |
| 2 | trunc 0, files 63 | trunc 2,945, files 63 |
| 3 | trunc 0, files 63 | trunc 2,922, files 63 |
| 4 | trunc 0, files 63 | trunc 3, **files 46** |
| 5 | trunc 0, files 63 | trunc 2,862, files 63 |
| 6 | trunc 0, files 63 | trunc 2,856, files 63 |
| | **0 truncations, 0 stalls** | up to 2,945 truncations, **2 stalls in 6** |

Read the two columns differently, because they carry very different weight.

The **truncation count** is the defect itself and it is not a rate: 6 of 6 against 6 of
6, zero versus thousands, with a mechanism that says exactly why. That is the result.

The **stall count** is the downstream symptom and 0-of-6 against 2-of-6 is a small
sample of an intermittent fault — precisely what gotchas 50-51 warn against reading as
decisive on its own. It is worth reporting because 2 in 6 reproduces the independently
recorded 2-in-8 baseline, so the control arm is behaving as the old binary did; it is
not worth reporting as if six runs had proved a rate.

One sub-pattern is worth keeping, because it inverts the obvious reading: **the control
runs that stalled have the FEWEST truncations** (3, against ~2,900 in the ones that
survived). A stalled run stops producing frames, so it stops accumulating them. A
truncation counter is a measure of exposure, not of damage — what decides a hang is
whether a dropped fence is one a thread is waiting on, and that is luck.

#### For the next port

`VdSwap` is not the only export of its shape, and the shape is the lesson: **a kernel
export that writes into a guest-owned buffer owns every byte of the reservation, not
just the bytes it has something to say in.** The caller's write pointer moves by the
reservation. Anything the kernel declines to write is still submitted, and in a
recycled buffer "declined to write" means "the previous frame's contents, executed".

Gotcha 5 has always said a stub must not fake success. This is its other edge: a stub
that does *part* of a job leaves a hole shaped exactly like real data, and the failure
surfaces in a subsystem it was never near.


### Finding 40 — the 1-in-40 crash does not reproduce, and looking for it found a memory-barrier hole

Task #11 carried a crash recorded at roughly 1 run in 20-40: guest thread `0xF2C`,
a `bctrl` in `sub_8284B568` at guest `8284B704` with `ctr = [r31+16] = 0`. It was
localised to the instruction and left to be characterised.

**The first thing to do with an inherited rate is to measure it again** (gotchas 50-51),
and that is the headline: **0 crashes in 20 runs at 120 s** on the committed binary,
with all 20 reaching the title screen. The old figure was taken before finding 39, when
a third to a half of runs stalled in the renderer's frame fence within the first minute;
whatever those runs were doing, the current ones do something else for two minutes and
do not die. The rate is not "improved" — it is unmeasurable at this sample size, and
saying which of those it is would need hundreds of runs nobody needs yet.

So this finding is a characterisation, not a fix, and the useful parts are what the
hunt turned up on the way.

#### What that thread is, and what the null actually means

`0xF2C`'s entry point is `0x8284B828` — and so is `0xF30`'s. **Two worker threads run
the same function**, which is the graphics driver's command-stream consumer: wait up to
30 ms on an event embedded in the job at `+0x3C`, `KeResetEvent`, then run the
interpreter `sub_8284B568`.

The interpreter walks a token stream out of a four-entry buffer ring at `job+0x5C`,
keeping its state in a *shared* object at `[job+0]`:

| offset | meaning |
|---|---|
| `+0x00` | `lwarx`/`stwcx.` spin lock |
| `+0x10` | the callback — **set only by a `0x8C000000` token** |
| `+0x14` | its user data (same token) |
| `+0x18` / `+0x1C` | iteration index / limit |
| `+0x20` / `+0x24` | base pointer / stream cursor |

A "run" token (bit 31 clear) sets index/limit/base and then dispatches through `+0x10`
`limit` times **without re-reading the stream**. So the callback it calls is whatever an
*earlier* token left in the object, and nothing ever resets it.

That makes the null precise: **the interpreter executed a run token before any token had
set a callback.** Not a corrupt pointer, not a vtable we failed to fill — a consumer
walking a stream that had not been published. Which is a producer/consumer ordering
question, and that is what sent the search into the recompiler.

#### The hole: `sync`, `lwsync` and `eieio` emitted nothing at all

Stock XenonRecomp lowers all three memory barriers to `// no op`. For the *hardware*
half that is correct on x86-64 and it is still the wrong answer, because the recompiled
image's ordering is not decided by the host CPU alone: every guest access is a plain C++
load or store through `base`, and a construct that generates no code constrains the host
compiler not at all. At `-O2` clang may move stores across a barrier the guest put there
to stop exactly that.

The idiom it breaks is the one this crash sits on:

```
    ...fill the token stream...
    lwsync                     ; publish those stores FIRST
    stw   r11, 0x58(r30)       ; then the tail index the consumer polls
```

The fix has to distinguish the two barriers, and getting it backwards is easy:

| instruction | orders | x86-64 TSO gives it? | lowering |
|---|---|---|---|
| `lwsync` | load-load, load-store, store-store | **yes, all three** | `atomic_signal_fence` — compiler barrier, no instruction |
| `sync` | the above **plus store-load** | **no** — the one x86 reorders | `atomic_thread_fence` — a real fence |
| `eieio` | stores to device memory | n/a | `atomic_signal_fence` |

51 `lwsync`, 11 `sync`, 14 `eieio` in this image. Details and the two related lowerings
that are *not* changed — `lwarx` is a plain load, which is only safe because `stwcx.` is
a `__sync_bool_compare_and_swap` and therefore a full barrier — are in
`docs/xenonrecomp-upstream-bugs.md` §6.

**This is not credited with fixing the crash and must not be.** The baseline was already
0 of 20, so there is nothing for it to improve on. It was applied because an unsound
memory model is a defect on its own terms, and it was measured only for the absence of
regression, 20 runs a side at 120 s:

| | pre-barrier (`8807ed6`) | with barriers |
|---|---|---|
| crashes | 0 of 20 | 0 of 20 |
| reached the title screen | 20 of 20 | 20 of 20 |
| truncated indirect buffers | 0 | 0 |
| A1 gate: clean 84-deep prefix | 13 of 20 | 13 of 20 |
| A1 gate: position-71 permutation | 7 of 20 | 7 of 20 |

That last row is the reason this table exists. Four hand-run gates on the new binary
came out 3-permuted-of-4 against two clean runs earlier in the session, which reads as
an obvious regression and is exactly the false alarm finding 38 recorded (gotcha 86:
the control is the old binary run NOW). Here the control was **free** — the rate
measurement had already saved 20 full logs per arm, and gating those is one loop. Both
arms permute at 35%, identically. Positions 71-73 have been scheduling-sensitive since
finding 38 and still are.

Worth keeping as a habit: a long rate run leaves behind a pile of complete boot logs,
and every log-based gate you own can be replayed over them for nothing. Twenty-versus-
twenty beats four hand-run pairs and costs less.

#### Two instruments that should have existed

**The crash reporter never fired on its own signature.** Its "LIKELY null indirect call"
test required `si_addr == nullptr` *and* `ctr` inside the image — and when `ctr` is
literally zero the dispatch-table lookup is never reached, so the process jumps to host 0
and `si_addr` is whatever the lookup computed, not null. The report read as an ordinary
segfault at a strange address and said nothing about the `bctrl` two instructions above.
It now names `ctr == 0` explicitly, and a third branch for `ctr` outside the image
("a corrupt pointer, not an unrecompiled function").

Widening it is one line and worth nothing unproven, so `CZ_CRASH_TEST=nullcall` makes a
guest thread call through a zero `ctr` on purpose — gotcha 30 applied to a diagnostic
rather than a test. A silent diagnostic is worse than none, and this one had been silent
on the exact case it existed for.

**There was no disassembler.** Every question here — what writes `+0x10`, what the token
encoding is, how many threads share the object — is a question about the title's own
code, and reading the recompiled C++ answers it in translation. The host toolchain
cannot disassemble this image at all: no PowerPC target in `objdump`, no `-b binary` in
`llvm-objdump`, and `llvm-mc` silently loses instruction alignment at the first VMX128
encoding it does not know, which on this image is constantly and invisibly. A capstone
wrapper had been written from scratch twice in two sessions and thrown away both times;
it is now `tools/gdis.py`, and it is what made this finding cheap.

Its `--find-uses` mode is the part worth stealing: a 32-bit constant is never one
instruction on PowerPC, so grepping an image for its bytes finds data references and
misses every code reference. Reconstructing the `lis`+`addi`/`ori` pair finds them all —
including the `addi` spelling where the high half is one greater because the low half
has bit 15 set, which is half of all addresses and is easy to miss.

#### What is left of task #11

The site is understood and the rate is unmeasurable, so there is nothing to chase until
it reappears. If it does, the tooling is now in place to answer it in one run:
`CZ_JOBQ_PROBE=1` prints the interpreter's shared-object state and the token buffer it is
about to walk on every entry, and the last line before a crash is the fatal call. That
turns "characterise it" into a single reproduction rather than a session.


## 5. Where the boot currently stops


**What runs.** The ring buffer is initialised, consumed and reported on. Over a 120 s
run with `CZ_RING_TRACE=1`, after finding 39:

```
ring: pm4 packets=120473162 frames(XE_SWAP)=3773 draws=8103913 interrupts=13374
ring: indirect buffers truncated=0 | verify clean=0 dirty=0
```

120 M packets parsed, 3,773 frames (~31 fps), 8.1 M draws, 13,374 command-processor
interrupts delivered to the guest ISR — and **zero unknown opcodes, zero parser stalls,
zero out-of-arena stores, zero truncated indirect buffers**. The read pointer chases the
write pointer rather than sitting frozen, which is the health check that says the parser
is keeping up.

The per-frame figure is the one that moved: **2,148 draws/frame against the 122 the
same line reported before finding 39**, and against A1's title-screen ~2,540. The old
number was not a worse renderer, it was a run that spent most of its life on loading
screens because it kept stalling; a run that reaches the title screen and stays there
is drawing a real scene.

Read that "zero parser stalls" narrowly: it is a statement about the RING walk, which
reports a frozen cursor after 60 ticks. Finding 38 found the walk of the *indirect
buffers* the ring points at ending early on a regular basis — dozens of times a
minute — and reporting nothing at all, because that path had no diagnostic. Three
green counters and a silent one is not a clean bill of health; it is three counters
and a blind spot (gotcha 25's shape again).

That silent counter now has a name and sits on the same trace line, so it can never be
a blind spot again:

```
ring: indirect buffers truncated=0 | verify clean=0 dirty=0
```

**`truncated` must be 0.** Every nonzero value is a hang waiting for a thread to wait
on it. It was ~2,172 per 90 s run before finding 39 and is 0 after.

**The gate.** Superseded twice; see section 6 for the current numbers. As of finding
36 the A1 run is an exact **84-deep prefix of Xenia's 93** and the A5 run tracks A5 to
its last visible position (119) with **zero real mismatch windows** — every remaining
mismatch is a permutation of one name set, i.e. thread scheduling.

The earlier readings of this paragraph — "four real windows", then "one" — were each
accurate when written; they are listed in findings 35 and 36 with what closed them.
One of the four was never independent: two windows were the wake of a single displaced
`RtlNtStatusToDosError` (finding 35, gotcha 71 — count causes, not windows).

**Where it stops.** A healthy run reaches the **title screen** — 64 files through to
`prologue_menu\prologue_z01.big`, rendering at ~34 fps — and waits there for a button
press it never gets (finding 37, measured, not inferred). Given one, it advances to A1
position 85 (`XamShowDeviceSelectorUI`) and then stops at 86 on the phase 2 save-data
enumerate stubs, which is a gap we chose.

The load stall that used to prevent that in a third to a half of long runs is **fixed**
(finding 39): it was `VdSwap` leaving 52 dwords of its 64-dword reservation unwritten,
so the command processor walked the previous frame's packets and dropped the fence the
renderer waits on. 6 of 6 runs at 120 s now reach the title screen with zero truncated
indirect buffers. Finding 38 has the mechanism and the two wrong answers on the way;
the critical-section half of finding 37's first reading of it is retracted there (the
same contention is present in healthy runs).

**An intermittent crash, and a retraction of the first reading of it.**

Roughly **2 runs in 10 segfault within 20 seconds**; the rest reach the timeout having
delivered between 372 and 1,178 vblanks. The variance across surviving runs is itself
the signal: this is a race, and the pump thread — new, running guest ISR code, and now
also running the command processor — is the obvious suspect.

The retraction matters more than the finding. A first A/B at 25 s per arm showed the
baseline crashing, `CZ_NULL_PAGE_READABLE=1` (null reads allowed, writes trapped) still
crashing, and both `CZ_NULL_PAGE_READABLE=rw` and `CZ_PM4_NO_CP_INTERRUPT=1` surviving
— which reads as clean and decisive: *the guest performs a null write on the source-1
ISR path, and the console tolerates it.* It was wrong. Re-run at 50 s per arm, **all
four arms survive**, including the baseline that had just crashed three times
consecutively.

Gotcha 7 says every instrument needs its own control. This is the other half:
**against an intermittent failure, an arm is not a measurement — a rate is.** One run
per arm will confidently name whichever arm happened not to fire.

## 6. Status

- `tools/kernel_call_diff.py --xenia A1 --ours <log>` → **exact 84-deep prefix of
  Xenia's 93**, stopping before `XamShowDeviceSelectorUI`. Position 84 is
  `MmMapIoSpace`, the XMA context mapping — closed by finding 36.
- `--xenia A5 --include-high-frequency` → **tracks A5 to position 119, its last, with
  ZERO real mismatch windows.** `SET MATCH: every mismatch is a permutation. Exit 0.`
  The two surviving windows are permutations of one name set each, i.e. thread
  scheduling (finding 35). This is the first fully clean A5 gate this port has
  produced.
  Note the duration: at 30 s the run stops around position 114 and the XMA path looks
  unreached. It is slow, not blocked — gate at 90 s.
- **155 of 244 imports real; 89 generated honest-failure stubs.**
- PM4: zero unknown opcodes, zero stalls, zero out-of-arena stores, **zero truncated
  indirect buffers** (finding 39), read pointer chasing the write pointer. Both
  capture oracles pass — `pm4_packet_lengths.py` on 24,527,474 packets and
  `pm4_indirect_walks.py` on 28,727 buffers.
- **The load stall is fixed** (finding 39). 6 of 6 runs at 120 s reach the title
  screen; the control arm `CZ_NO_SWAP_PAD=1` still stalls 2 in 6.
- **Stability: 0 crashes in 20 runs at 120 s, all 20 reaching the title screen**
  (finding 40). The 1-in-20-to-40 crash recorded before finding 39 does not reproduce.
- Memory barriers are real now: `lwsync`/`eieio` are compiler barriers and `sync` is a
  fence, where all three previously emitted nothing (upstream bug 6, gotcha 92).
- `cz_runtime --smoke` still passes: the phase 0.2 link gate is intact.
- Audio: the render-driver pump runs at 5.333 ms/frame (256 samples x 6 channels),
  the guest submits frames, and the peak amplitude through the boot is 0.0000 —
  a live driver carrying silence, which is what a boot with no audio content should
  look like. No output backend and no XMA decoding yet.

Next, in order:

1. **Prove the still-unexercised imports** (gotcha 67). Finding 34's eight remain
   predictions; `XamTaskSchedule` in particular runs guest code on a new thread and
   has never done so. Of finding 36's seven, **five run and two do not** — both
   teardown paths, `XAudioUnregisterRenderDriverClient` and `XMAReleaseContext`,
   because the boot never shuts the audio device or the stream table down.
2. The save-data layer proper — `XamContentCreateEnumerator`, `XamEnumerate`,
   `XamGetPrivateEnumStructureFromHandle`, `XamContentCreateEx`, `XamContentClose`.
   Deliberately left out of finding 34: they are the phase 2 file layer, not the
   message/device mechanism.
3. Audio output and XMA decoding, when phase 5 arrives. The kick bitmap at
   `0x7FEA1A80` currently lands in ordinary flat memory and is inert; a real decoder
   needs that aperture trapped as MMIO or the kick is written and never noticed.
