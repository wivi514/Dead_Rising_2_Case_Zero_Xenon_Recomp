# Part 85 kickoff — MILESTONE D IS COMPLETE; the release needs E next

> **THIS IS THE LIVE HAND-OFF**, superseding `part84-kickoff.md`.
>
> **The subject is THE RELEASE.** `docs/release-plan.md` is the programme; its **§9 is the
> execution record** and **§9.7 is part 84** — every measurement in this hand-off lives
> there in full.
>
> | document | what it is |
> |---|---|
> | **`release-plan.md` §9.7** | **part 84: milestone D complete — D.2, D.4, D.3, every gate** |
> | `release-plan.md` §3.E | milestone E, the remaining work, as planned |
> | `docs/windows-build-setup.md` | the Windows runbook — now carries the XenosRecomp tree and the pull-verification rule |
>
> Lessons: gotchas **501-503**.

---

## 0. THE ONE THING A NEW SESSION MUST KNOW

**A shipped build can now translate its own shaders.** No Python, no shell, no XenosRecomp
executable, no dxc CLI on the player's machine:

* **D.2** — `gpu/shader_translator.{h,cpp}` translates raw microcode to SPIR-V + sidecar
  in-process, **byte-identical** to the offline pipeline (449 of 449, all 898 files), in
  2.6 s against the shell's 51 s. The gate is `cz_runtime --translate-shaders
  ~/DR2CZ-troubleshooting/ucode-dumps <out>` + `diff -r` against `assets/shader_spv`, and it
  must run after any change to EITHER implementation.
* **D.4** — a hash the cache lacks is translated at first sight on a worker, persisted, and
  registered live. **This is where every vertex shader comes from** (the disc holds none in
  usable form). Empty-vertex-half gate: crowd at 8,110 draws, `no translated shader` = 0,
  45 shaders at 18-70 ms each. `CZ_VK_NO_SHADER_JIT=1` is the arm.
* **D.3** — `gpu/shader_prebuild.{h,cpp}` builds the pixel half from the player's disc:
  **1,265 of 1,265 in 9.0 s**, resumable (marker files; a populated dev cache is never
  touched), automatic on a renderer boot when the cache is missing. Crown gate: disc cache
  only → crowd, 0 missing, and the two pixel first-sights are exactly the two hashes D.1
  found absent from the disc.
* **Cross-platform**: Windows produces the byte-identical 1,265 — **a shader cache is one
  artifact, not one per OS.**

Milestones **A, B, D complete; D.1 done in part 82.** `ssh czwin` reaches the Windows box —
read `docs/windows-build-setup.md` first, including its new pull-verification rule
(gotcha 502: a silenced chained `git pull` failed and two build claims were made against a
stale tree before a missing CLI flag gave it away).

## 1. WHAT TO DO NEXT, IN ORDER

1. **E — packaging, CI, the release itself** (`release-plan.md` §3.E). E.2's Linux script
   exists in pieces (part 82's five commands); a Windows artifact is now buildable and needs
   its equivalent. The bundle must carry `dxcompiler.dll` / `libdxcompiler.so` in `lib/` —
   the translator dlopens it and refuses loudly if absent.
2. **The graphical first-run progress screen** (owed from D.3): "Preparing shaders" is
   console lines; §2.3 wants a window. Do it as part of E.4/E.5 polish, where the first-run
   experience is assembled end to end.
3. **The in-process STFS extract** (§2.3 step 2): `first_run` still refuses with the
   `extract_stfs.py` command instead of running the extraction itself. The Python reader is
   the reference; the port is mechanical.
4. **Ship a pre-warm pipeline-key file** (carried from part 84's kickoff): a few tens of KB
   from a full playthrough gives players a warm start. Needs an operator playthrough with
   the key recorder on, and an artifact (E) to ship it in.
5. **C — macOS** remains blocked on hardware; C.4's Apple account is OPTIONAL.

## 2. WHAT IS OWED TO THE OPERATOR

* **Nothing measured.** Every gate in part 84 was run and recorded.
* An operator sighting of first-sight translation in real play would be a nice
  confirmation (a new area with `CZ_SHADER_SPV` pointed at a copy of the cache, watching
  for one `[vk] first-sight translation:` line and no visual gap longer than a beat) — but
  the headless gates already cover the mechanism; do not spend a session on it.

## 3. THINGS PART 84 SETTLED THAT SHOULD NOT BE RE-DERIVED

* **DXC's API == DXC's CLI, byte for byte**, same arguments, with or without a source name.
* **The JIT never touches the standard path**: full cache + JIT on = 0 first-sight, 0
  skips; and a dev cache (no marker files) never triggers the disc pass — 898 files before
  and after.
* **The two implementations are deliberate duplicates** (Python pipeline = dev tool with
  arm-cache hooks; C++ = shipped path), kept honest by the byte-identity gate. When one
  changes, run the gate; a disagreement names the shader.
* **The six variant arm caches do not gain first-sight entries** — a shader born at run
  time reads `no translated shader` under `CZ_SHADER_SPV=<arm cache>`. Dev-only, known,
  not a defect.

## 4. GATES PART 85 INHERITS

Everything part 84 inherited (part84-kickoff §4), plus:

* `cz_runtime --translate-shaders` over the 449 dumps diffs clean against
  `assets/shader_spv` — run after any change to the translator, the Python pipeline, or
  the sidecar format.
* `cz_runtime --build-shader-cache` into a fresh directory: 1,265, 0 refused, 0 failed;
  the 343 canonical overlaps byte-identical.
* The empty-vertex crown gate: pixel-only cache → crowd with `no translated shader` = 0.
* The prebuild must print `N translated, M already present, 0 failed` and write
  `disc_prebuild.done` only on a clean pass.
