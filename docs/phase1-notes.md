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

## 5. Where the boot currently stops


**What runs.** The ring buffer is initialised, consumed and reported on. Over a ~25 s
run with `CZ_RING_TRACE=1`:

```
ring: kickedWptr=000031E4 (dev+10956)  writebackPtr=BC739380 (dev+10896,
      registered BC7393BC)  [wb+0]=00000C35  mmio CP_RB_WPTR=000031E4
ring: pm4 packets=1271801 frames(XE_SWAP)=563 draws=68588 interrupts=1235
```

1.27 M packets parsed, 563 frames, 68,588 draws, 1,235 command-processor interrupts
delivered to the guest ISR — and **zero unknown opcodes, zero parser stalls, zero
out-of-arena stores**. The read pointer chases the write pointer rather than sitting
frozen, which is the health check that says the parser is keeping up.

**The gate.** `--xenia A1` (masked): **PREFIX MATCH, 56 of 93**, up from 43. The entire
Vd block — positions 28 through 56, `VdInitializeEngines` through
`XAudioRegisterRenderDriverClient` — matches hardware element for element, and it got
there partly on stubs.

**The divergence at 57** is `RtlCompareStringN`, a generated stub, arriving where
hardware calls `XamGetSystemVersion`. Everything past it is the XAM/frontend and
network surface, where our sequence holds mostly the same names in a different order.
That is the next subsystem, not the next bug.

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

- `tools/kernel_call_diff.py --xenia A1 --ours <log>` → **PREFIX MATCH, 56 of 93**,
  diverging at `XamGetSystemVersion` vs our `RtlCompareStringN`.
- 118 of 244 imports real; 126 generated honest-failure stubs.
- PM4: zero unknown opcodes, zero stalls, zero out-of-arena stores, read pointer
  chasing the write pointer.
- `cz_runtime --smoke` still passes: the phase 0.2 link gate is intact.
- **Not stable: 6-7 runs in 10 crash within 20 s** (re-measured; the earlier "~2 in
  10" is retracted — finding 26). Two distinct faults: a null-pointer walk on the main
  thread, which dominates, and the poison indirect call on the pump thread, now
  declined.

Next, in order:

1. **The null-pointer walk on the main thread** — the dominant crash. The reporter
   gives its 17-frame back-chain rooted at the boot path; work down it for the frame
   that produces the null, then find which import handed it over. Finding 15's first
   failure mode is the prior.
2. **The XAM/frontend surface** — everything past gate position 57.
3. The early `RtlNtStatusToDosError` the A5 gate reports at position 19.
