# Runtime bring-up plan — from recompiled image to pixels

Written 2026-08-04, immediately after the first successful recompilation and **before any
ground truth exists**. That is the main caveat on everything below: the phase *shapes*
are lifted from two ports that worked, but the numbers that would size each phase for
this title have not been measured yet. Sections marked **(unmeasured)** are structure
only; do not treat them as estimates.

The playbooks: `~/GithubRepo/Fable2XenonRecomp/docs/runtime.md` (the deepest write-up)
and `~/GithubRepo/Asuras_Wrath_Xenon_Recomp/docs/runtime-plan.md` (the same plan applied
a second time). `runtime/` is ported from Fable 2 module by module; UnleashedRecomp is
GPLv3 and is a **structural reference only** — guest structs come from XenonRecomp's
`XenonUtils/xbox.h` (MIT).

No phase starts on guesswork; each has a ground-truth gate that says "done".

---

## Phase 0 — make the image compilable and honest *(prerequisite)*

**0.1 Close the 42 unrecognized-instruction sites.** Six mnemonics — `lhbrx` (30),
`stfsux` (5), `vsubuws` (4), `vspltish`, `vpkuwum`, `vadduhs` — implemented in
`~/GithubRepo/XenonRecomp`. Implement all six, not just the ones a capture proves are
executed: the marginal cost is minutes each, and "never executed" only ever means "never
executed on the paths captured so far".

`lhbrx`'s 30 sites are worth a second look while implementing it — a byte-reversed
halfword load that frequent smells like an endianness helper in the `.big` archive
reader, i.e. on every asset load path.

**0.2 First real compile.** The 227 TUs (154 MB) have never been fed to a C++ compiler.
Stand up the CMake skeleton, build `ppc/` as a static library, and burn down what falls
out. Both earlier ports hit link-scale problems at this step and both solved them; at
57,728 functions this image is smaller than either, so expect less.

**Gate:** recompiler log is completely clean (zero unrecognized, zero switch errors —
switch errors are already zero), and a smoke `main` that walks the whole
`PPCFuncMappings` table links and runs, forcing the linker to resolve every generated
symbol.

## Phase 1 — kernel HLE and guest bootstrap

Memory map, o1heap guest arenas, thread creation, events/APCs, NT timers, TLS, the VFS
over `assets/game/`, and XAM stubs. Written against A1's call order.

Two rules that are not negotiable, both learned expensively:

- **Stubs fail honestly, never fake success.** Fable 2's faked XMA context cost weeks.
- **A stub that returns an error but leaves its out-parameter untouched is worse than no
  stub** — the guest frequently ignores the status and reads the buffer anyway.
- **Put the guest arenas where the console puts them.** Games select between the 4 KB-
  and 64 KB-page virtual regions with `MEM_LARGE_PAGES` and then reason about the
  addresses; matching the console's map also makes our addresses directly comparable to a
  capture's. And every size the kernel *reports* must be rounded the way the console
  rounds it, not just allocated correctly.

**Gate:** `tools/kernel_call_diff.py` (port from Asura's Wrath) against A1 — our call
sequence is a prefix-match of hardware's out to the title screen.

## Phase 2 — the VFS and the `.big` archives *(unmeasured)*

Case Zero's content is `.big` containers plus `.bct` textures and `.bcf` fonts, with at
least one runtime-constructed path (`anm_%s.big`), so the VFS handles arbitrary paths
rather than a fixed manifest. The container format is unknown and will need cracking;
Fable 2's `.bnk` work in that repo's `tools/` is the closest model.

**Gate:** the file-open sequence in our log matches A1's, in order, out to the title
screen.

## Phase 3 — window, present seam, input

SDL window, the present seam every later stage goes through, keyboard + XInput gamepad.
Cheap, and it makes every later phase observable.

## Phase 4 — GPU command processor

Execute the real PM4 stream: register file, fences, waits, indirect buffers, CP
interrupts. Gate it offline first with a `pm4_replay` target that walks the B1 `.xtr`
captures and reports zero unknown opcodes and zero desyncs, before any of it runs live.

Three stream semantics that cost the earlier ports real time and are likely to recur:
the headerless first-IB preamble; ring size units and wrap behaviour; and **a command
stream carrying the answers to its own waits** — the D3D driver's GPU waits poll a
retired-fence counter and a consumed-to pointer that no runtime could honestly invent,
and both are written by `EVENT_WRITE_SHD` packets the guest itself put in the ring.

Also: **a packet's contract can include *when* it runs relative to its neighbours.**
Asura's Wrath's graphics ISR reads a callback pointer out of the scratch mirror that the
stream arms immediately before the `INTERRUPT` packet and poisons immediately after, so
deferring the completion signal to the end of the walk — free, and what Fable 2 does —
calls the poison.

**Gate:** `pm4_replay` over every `.xtr` capture: zero unknown opcodes, zero desyncs,
swap count matching the capture's frame count.

## Phase 5 — renderer *(unmeasured; verify the shortcut first)*

**Before planning this phase, verify the shader banks.** Case Zero ships
`data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big` as loose files. If those
hold raw Xenos microcode they feed XenosRecomp almost directly, and this phase looks
nothing like Fable 2's (which needed a whole `.sbk` extraction pipeline and, in the
interim, a hand-written software rasterizer). This is the single biggest open question in
the project and it is answerable in an afternoon with no capture at all.

Fallback that always works: a runtime `SHADER_DUMP` capture of whatever the guest hands
the driver.

**Gate:** per-pixel diff against the D-series screenshots at the same frame.

## Phase 6 — audio

`XAudioRegisterRenderDriverClient` / `XAudioSubmitRenderDriverFrame` pump at 48 kHz, XMA
decode via ffmpeg. Asura's Wrath measured that title using **all 32 hardware XMA
contexts** — a partial context implementation did not survive a cinematic. Case Zero's
usage is unmeasured; A2 will show it.

## Phase 7 — Bink

Hook **`BinkDoFrame(HBINK)`**, the middleware's own API, not the game's wrapper around
it: RAD's decoder is linked into the XEX, so that seam is identical in every title that
links it, whereas a wrapper hook is engine-specific.

And gate it with a **dense** oracle. Asura's Wrath's decode looked right, advanced its
frame counter, and was one display frame stale in *every* frame — visible only as ~60 bad
8×8 blocks in the one moving corner of a mostly-static picture, wearing the diff signature
of a localised codec bug. Only a per-pixel diff against ffmpeg caught it. Bink's own
`BINKFRAMEBUFFERS.FrameNum` is the next-write index, not the last.

## Phase 8 — saves and content

`XamContentCreateEx` → `savedrive:` → `\Device\Content\N\`, N incrementing per mount.
Shape from A3.

---

## Measurement discipline, from day one

- A/B with **same-binary arms** (`ab_gate.py`, port from Fable 2).
- **A probe expensive enough to stall the game manufactures the stability it reports** —
  every instrument needs its own control.
- **No silent caps.** If a comparison bounds coverage (top-N, sampling, no-retry), log
  what was dropped; silent truncation reads as "covered everything" when it didn't.
- **Retract in place.** When a stated finding turns out to be an artifact, say so where
  it was claimed and explain the artifact.
