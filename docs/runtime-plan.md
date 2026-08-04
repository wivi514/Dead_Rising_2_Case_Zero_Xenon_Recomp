# Runtime bring-up plan — from recompiled image to pixels

Written 2026-08-04 before any ground truth existed; **revised the same day** once round 1
was delivered. The findings ledger (`docs/xenia-capture-analysis.md`) is now the
authority on measured numbers — where this plan and that ledger disagree, the ledger
wins.

Two phases changed shape outright when the captures landed: **phase 5 (renderer)** got
its input settled, and **phase 7 (Bink) was deleted** — this game contains no Bink at
all. Both are marked below.

The playbooks: `~/GithubRepo/Fable2XenonRecomp/docs/runtime.md` (the deepest write-up)
and `~/GithubRepo/Asuras_Wrath_Xenon_Recomp/docs/runtime-plan.md` (the same plan applied
a second time). `runtime/` is ported from Fable 2 module by module; UnleashedRecomp is
GPLv3 and is a **structural reference only** — guest structs come from XenonRecomp's
`XenonUtils/xbox.h` (MIT).

No phase starts on guesswork; each has a ground-truth gate that says "done".

---

## Phase 0 — make the image compilable and honest *(prerequisite)*

> **Status 2026-08-04: 0.1 still open; the coverage half of 0.2's prerequisite is done.**
> The C1/C2 forwards oracle recovered 110 missing entry points and the image now sits at
> **57,837 functions with zero switch-boundary errors and zero `// ERROR:` comments**
> (finding 5). What remains here is the instruction work and the first compile.

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
57,837 functions this image is smaller than either, so expect less.

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

## Phase 2 — the VFS and the `.big` archives

**The container format is cracked** — `docs/big-archive-format.md`, confirmed both from
the archives themselves and independently from A5's read pattern. No reverse-engineering
phase is needed here any more.

What the format implies for the VFS: the runtime probes an archive's header, reads its
index, then seeks to individual entries, so we need **random access within an archive,
not sequential streaming**. Paths are constructed at runtime (`anm_%s.big`), so the VFS
handles arbitrary paths rather than a fixed manifest. Gameplay opens 433 distinct
archives across 23,965 file opens (A2).

**Gate:** the file-open sequence in our log matches A1's, in order, out to the title
screen.

## Phase 3 — window, present seam, input

SDL window, the present seam every later stage goes through, keyboard + XInput gamepad.
Cheap, and it makes every later phase observable.

## Phase 4 — GPU command processor

> **Prerequisite that does not exist yet: an `.xtr` decoder.** Findings 9 and 10 both end
> at "needs the decoder" — the determinism baseline is unmeasured and no gate can be
> built without one. Write it first. The good news is the operator fixed Xenia's 2 GiB
> `.xtr` cliff at source (finding 9), so gameplay captures are unbounded and the B2
> stream is 7.95 GiB of real data.

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

## Phase 5 — renderer  *(input settled)*

**The shader question is answered, and not the way the bootstrap doc guessed.** The loose
disc banks are `.vo` shader-object containers whose payloads are *not* the microcode the
guest submits — they carry build metadata including `.updb` debug paths, and share only
background-noise n-gram overlap with the real thing (finding 6). That shortcut is gone.

It does not matter, because Xenia's `dump_shaders` delivered the real input:
**455 distinct raw Xenos microcode blobs** — 120 frontend/menu (A1) and 335 gameplay
(A2) — as `*.ucode.bin.{vert,frag}`, each with disassembly and Xenia's own D3D12
translation alongside. That is exactly what XenosRecomp consumes, so no runtime
`SHADER_DUMP` hook is needed and this phase starts with its inputs in hand.

**Gate:** per-pixel diff against the E-series screenshots at the same frame.

## Phase 6 — audio

`XAudioRegisterRenderDriverClient` / `XAudioSubmitRenderDriverFrame` pump at 48 kHz, XMA
decode via ffmpeg. Asura's Wrath measured that title using **all 32 hardware XMA
contexts** — a partial context implementation did not survive a cinematic. A2 shows Case
Zero using contexts **0–17+**, so plan for the full set rather than the observed maximum;
the observed maximum is a property of the drive, not of the title.

## Phase 7 — movies  *(was "Bink"; rewritten — there is no Bink here)*

**Case Zero contains no Bink and no `.bik` files** (finding 7). Both template ports hook
RAD's `BinkDoFrame`, and that entire approach is inapplicable. Cinematics stream through
an in-house "Movie Player Object" reading `.big` cinematic archives (`ratinglogos.big`,
`700_prologue_intro.big`, `cinematics.big`).

This is a *harder* phase than it was on either template port, not an easier one: the
codec is unknown and has to be reverse-engineered out of the `.big` payloads, and there
is no middleware API to hook — the seam will be engine-specific, which is exactly the
kind of hook Asura's Wrath's gotcha #22 warns is fragile. It also means the movie era is
not the cheap early visible win it was there.

The one thing that does transfer is the gating discipline. Asura's Wrath's Bink decode
looked right, advanced its frame counter, and was one display frame stale in *every*
frame — visible only as ~60 bad 8×8 blocks in the one moving corner of a mostly-static
picture, wearing the diff signature of a localised codec bug. Only a per-pixel diff
against a reference decode caught it. **Gate any video work per-pixel, never on "it looks
right" or on a frame counter the decoder itself maintains.**

## Phase 8 — saves and content  *(shape measured)*

`XamContentCreateEx(…,"save",…,flags=0x1012)` → symbolic link `save:` →
`\Device\Content\1\` → `NtCreateFile(save:\DR2P000.DSF)` → **one `NtWriteFile` of
0x4A000 = 303,104 bytes** → `XamContentClose`. Load-back goes via
`XamContentCreateEnumerator` → re-mount → re-open (finding 12).

The whole save is a single write, which makes this materially simpler than Asura's
Wrath's. The physical save file was delivered with the capture, so the `.DSF` format can
be reverse-engineered offline without re-running anything.

---

## Measurement discipline, from day one

- A/B with **same-binary arms** (`ab_gate.py`, port from Fable 2).
- **A probe expensive enough to stall the game manufactures the stability it reports** —
  every instrument needs its own control.
- **No silent caps.** If a comparison bounds coverage (top-N, sampling, no-retry), log
  what was dropped; silent truncation reads as "covered everything" when it didn't.
- **Retract in place.** When a stated finding turns out to be an artifact, say so where
  it was claimed and explain the artifact.
