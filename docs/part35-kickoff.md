# Part 35 hand-off (for part 36). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part34-kickoff.md` for "where the
port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays its cinematics through; shadows
are fixed and operator-confirmed (part 34). Part 35 was an operator-driven re-ask of
the parked picture items and it reshaped the backlog: **item 3d (NPC part meshes) is
closed** (the parts were the shader-cache gap), **item 00i (LOD placeholder pop) is
captured and one Xenia look from a verdict**, and the wrong-texture reports collapsed
into ONE new top item — **the striped-material class (item 0s)** — with five theories
killed by measurement in a single session and the next move named: trace the WRITER of
a junk sheet, because every reader of the bytes is exonerated. A real (if
symptom-neutral) VFS defect was fixed on the way: positional file IO is now atomic per
handle, as NT requires.

## WHAT PART 35 DID — do not rebuild any of this

Full record: `docs/phase5-notes.md` §6bi; captures + live dumps in
`~/DR2CZ-troubleshooting/part35-item1-operator/` (indexed there). Gotchas 285-286.

* **Item 3d closed**: Dick renders whole on two binaries; the missing-parts symptom
  was the shader cache, as the item's own re-test note predicted.
* **Item 00i captured**: flat colour panels at distance -> full siding close
  (captures 30631/30807). The far state is the census's single-repeated-block
  flat-colour upload class. Owed: ONE Xenia look at the same street's promotion
  distance.
* **Item 0s created — the striped-material class.** Read its entry in
  `open-items.md` before touching anything picture-related: five refutations are
  recorded there (shadow term; VFS IO race — overlap counter 0; the pickup-atlas
  misattribution; snapshot age fallback; cache staleness — content guard 4 of 92.7M
  hits). Guest memory genuinely holds the garbage at blotch time; the affected
  textures include CPU-composed impostor sheets that exist nowhere on disc and are
  never resolve destinations.
* **The VFS fix (d65874d)**: per-handle mutex making seek+read/seek+write atomic,
  `CZ_FILE_RACY=1` the control arm, overlap counter printed live (gotcha 284 shape).
  Its registered prediction FAILED and is retracted in §6bi — the fix stays on
  correctness, credited with nothing visible.
* **The live-dump protocol** (gotcha 285): operator presses F9 and STANDS STILL;
  `live_texdump.py` (session scratchpad — REWRITE IT, it's tmpfs) reads every texture
  the census names out of the live process within seconds via `process_vm_readv`.
  Minutes late = recycled memory = evidence for any theory you brought.

## READ THIS BEFORE MEASURING ANYTHING

* Everything from parts 26-34 stands, plus: **a live-process dump has a time**
  (gotcha 285), and **the blotch class needs writer-side instruments now** — another
  arm on the sampling path is a spent shape (gotcha 286, five worked examples).
* The junk-scorer used in part 35 flags any greyscale-with-extremes texture — every
  neutral AO/lightmap scores as "junk". Triage only; decode and LOOK before claiming.

## WHERE TO START

1. **Item 0s, the writer hunt**: `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` on the outdoor
   route first — the engine narrates streaming and may name its impostor/composite
   system outright. Then a write watch on one sheet's page when its fetch first
   appears (the `guest_probe` machinery is the worked example) to name the composing
   function for `gdis`. Then read that function's SOURCES — the standing suspect is
   a resolve's guest-memory copy, which hardware writes and we never do (part 25's
   "resolve pixels are never written back" note).
2. **The Xenia one-look for 00i** (promotion distance of the flat-panel shop) — ask
   the operator; retires or convicts in one screenshot.
3. **The census's unclaimed sub-defects** (in item 0s): 231 colour fetches served by
   a DEPTH resolve snapshot; the garbled subtitle text (f24288).
4. The rest of `docs/open-items.md` (cube-decline s3/s4 duplicate, mipmaps, LUT
   blending) and `docs/perf-cpu-plan.md`'s CPU/GPU overlap.

## Gates, on this binary (d65874d)

* `--smoke` OK. No renderer change this part; part 34's gates stand.
* The VFS change adds no kernel calls; A5 diff untouched by construction but NOT
  re-run this part — re-run before any claim resting on it.
* Shader cache: 417, `no translated shader` 0 in all part-35 sessions.
