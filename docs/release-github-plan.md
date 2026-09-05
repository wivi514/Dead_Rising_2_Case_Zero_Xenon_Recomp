# The GitHub release — plan

**Status: EXECUTING — §0, §2, §3.3, §3.4 and §4 DONE 2026-09-05 (same-day session;
§6 below is the execution record). WHAT REMAINS NEEDS THE OPERATOR: §3.1 (bundle
save round-trip, both platforms), §3.2 (KB/M from the bundle — first time the
whole native path incl. first-run overlays exists in an artifact), then §5's
tag + Release creation (no `gh` here) and the visibility flip.**

~~**Status: PREPARED 2026-09-05, not started.**~~ Operator instruction, closing the KB/M
fix arc: *"Now we switch to the release everything is good enough so prepare a plan for
release we can put on github."* This plan is the successor to `part92-kickoff.md` §1
item 1 (the deferred release board) and builds on `release-plan.md` §9.8 (milestone E's
execution record and its owed list). It covers everything between the current tree and
a public GitHub repository carrying a tagged release with both artifacts attached.

**Where the artifacts stand:** both exist and were gated in part 85 —
`CaseZeroRecomp-linux-x86_64.tar.zst` (26 MB) and `CaseZeroRecomp-windows-x86_64.zip`
(21 MB) — and the operator completed the whole game on the Windows bundle. But they
predate parts 86-96 ENTIRELY: the save fix, the launcher, level cap 50 + skills, the
CW pump stack, parallel record, deferred clears, live resolution apply, the whole
native KB/M build (input, icons, device-follow, MASH prompt), MSAA, and the texture-LOD
fixes. Nothing ships until both are rebuilt at head and re-gated.

**The repo is already on GitHub** (`wivi514/Dead_Rising_2_Case_Zero_Xenon_Recomp`,
origin reachable, CI green on both legs). The release is therefore: close the gaps
below, rebuild, verify, rewrite the front page, tag, attach, flip visibility.

---

## §0 The one discovered blocker: THE OVERLAY GAP (new engineering — do this first)

**A bundle built today ships WITHOUT the patched-asset overlays, silently.** Verified
by grep before writing this plan: neither packaging script, nor the container gate,
nor `first_run.cpp` mentions `assets/game_patched` or `assets/game_kbm` — no road.
Those overlays are generated in the DEV tree by `tools/gen_pc_options.py` and
`tools/gen_kbm_icons.py` (Python + PIL) FROM the game data, and they carry
Capcom-derived bytes (repacked banks are mostly Capcom's own entries), so they can
neither ship in the artifact (the project's own line: *"the transform is code, the
content stays Capcom's"* — gen_pc_options.py's docstring) nor be regenerated on a
player's machine that has no Python.

What a player loses without them, all silent fall-throughs in `vfs.cpp`:
* the resurrected PC options screen (part 60) — the Visuals panel's in-game host,
* PRESS ENTER at the title, and every string edit since (MASH, A / D KEYS),
* ALL 25 keyboard prompt icons + device-follow (`glyph_swap.bin`) — KB/M *input*
  works (it is code), but every prompt shows pad art. KB/M is now a headline
  feature; shipping it half-present is the gotcha-5 shape.

**The fix: move overlay generation into the first-run flow**, exactly the road
`host/stfs_extract.cpp` proved in part 85 (in-process port, byte-identical to the
Python reference, which stays as the dev tool and oracle):

1. **Port the LZX verbatim-block encoder** (`lzx_encode_stream`) to C++. It is one
   self-contained function (greedy LZ77, 32 KB window, canonical Huffman, VERBATIM
   blocks only); the part-60 ladder already established WHY it must be this exact
   shape (the guest crashes on degenerate streams). The decoder side already exists
   in C++ (`tools/big_decompress` links XenonRecomp's own LZX).
2. **Port the two bank transforms**: the fecmn.big repack with the rewritten
   `options_pc.txt` (a text transform, shipped as data or code — our text), the
   `.bcs` string-table rebuild (trivial struct work, format in both Python tools),
   and the fecmn.tex glyph patch + `glyph_swap.bin` emission. **Our chip art ships
   pre-rendered**: bake the 25 key-cap chips at PACKAGE time into raw DXT5 texel
   blobs (they are OUR art, no Capcom bytes — PIL stays a dev-only dependency);
   first run composes them into the player's own bank and reads the PAD texels for
   glyph_swap from the player's bank, so no Capcom byte ever ships.
3. **Wire into first run** after the STFS extract / alongside the disc shader
   prebuild, under the existing progress window. Regenerate whenever absent;
   `CZ_NO_PATCHED_ASSETS=1` / `CZ_NO_KB_PROMPTS=1` stay the arms.
4. **The gate is free and exact**: the dev tree has the Python outputs — the
   in-process generator's output must be BYTE-IDENTICAL to them (the stfs_extract
   precedent). Extend the clean-container gate to require the overlays exist after
   its first-run section and spot-check one hash.

**The shortcut, stated so the choice is the operator's and not an accident:** ship
`assets/game_patched` + `assets/game_kbm` + `glyph_swap.bin` inside the artifact.
One packaging-script line, zero new code — and it puts repacked Capcom banks and
raw Capcom texels into a public download, a materially worse legal shape than the
recompiled binary alone, and one this project explicitly avoided in part 60. Not
recommended; recorded because it exists.

## §1 Decisions the operator owns — ALL FOUR ANSWERED 2026-09-05, same day:
## **v1.0.0 · attach both artifacts · keep MSAA 2x default · glibc floor as a
## known limitation.** Nothing in this section blocks anything anymore.

1. **Version/tag.** Suggest `v1.0.0` — the game is completable start to finish on a
   shipped bundle (part 85's strongest verdict). `v0.9.x` if they want a soft-launch.
2. **Attach the artifacts to the GitHub Release?** The binary contains the recompiled
   image (derived from Capcom's XEX). Precedent: hedge-dev's UnleashedRecomp is public
   on GitHub with exactly this shape — binary with recompiled code, player supplies
   the game data, honest refusal without it (ours: `host/first_run.cpp`). The source
   REPO is clean either way (447 tracked files, all text/config, zero game bytes —
   verified below). Recommend: attach, with the README's legal note stating the
   player must own the game. Their call to make explicitly.
3. **MSAA 2x default in the bundle** — part 93's owed decision, needs their
   AA-vs-cost read on THEIR machine (+0.85 ms GPU at 1440p on the dev box).
   `CZ_VK_MSAA=0` remains the arm either way; `cz_defaults.env` is where the
   decision lands.
4. **glibc floor**: ship v1.0 with the floor as a stated known limitation (the
   packaging script already prints it), AppImage later — recommended — or block the
   release on an old-base build now.

## §2 Rebuild both artifacts at head

* **Linux** (self-serve): matched-configure Release build, `release_text_identity.sh`,
  `release_package_linux.sh`, `release_gate_clean_container.sh` (must print GATE
  PASSED; as of part 85 it runs the whole first-run flow + a DXC translation
  in-container — extend it per §0.4).
* **Windows** (czwin): push → `git pull --ff-only` → build through `vc.bat`. This is
  the FIRST czwin build of parts 87-96's SDL-side input code (native_kbm.cpp, the
  keystroke seam, device-follow's process-memory scans) — expect MSVC-side friction
  and budget for it; do not discover it during an operator sitting. Then
  `release_package_windows.ps1` (its gate RUNS the staged exe and builds all 1,265
  disc shaders). Both packagers preserve player assets themselves (part 85's
  repackage-wipe lesson — already fixed and gated).

## §3 Verify the rebuilt bundles (the owed verifications, now testable)

1. **Windows bundle save round-trip** — §9.8's release blocker: broken in part 85's
   bundle, fixed by part 86's save relocation, but never verified ON A BUNDLE since.
   Make a save, quit, relaunch, load. Do the same on Linux.
2. **KB/M from the bundle** — first time the whole native path (input + first-run
   overlays from §0) exists in an artifact. One operator sitting covers this and the
   save check together; the struggle prompt (MASH, A↔D) is the newest visible.
3. **Prewarm seed vs MSAA** — the shipped `prewarm.keys` (1,365 keys) was harvested
   at single-sample. Pipeline keys carry render state, so if MSAA 2x ships default,
   check the cold-boot seed count doesn't collapse (part 85 baseline: 757 built up
   front). If it does, one operator sitting with the recorder re-harvests — the same
   free road as part 85.
4. **Standing gates**: `no translated shader` = 0, the shader-cache name-diff, sync
   validation unchanged (6 `topology-08773` and nothing else), `--smoke` on both.

## §4 The public front page

1. **Rewrite the root `README.md`.** It is the day-1 dev README (status stops at
   phase 0!). The public one needs: what this is (one paragraph), a screenshot or
   two (operator F9s — ask for a nice one), player quickstart (download, drop the
   STFS package in `assets/package/`, run — first run extracts and builds shaders
   itself), requirements, features (60 fps, KB/M with native prompts, live
   resolution, MSAA, launcher…), known limitations (glibc floor, no macOS, the hair
   flicker, trial-vs-full package note), build-from-source pointer (the pipeline
   needs the player's own game data; CI proves the host code builds), credits
   (XenonRecomp/XenosRecomp — hedge-dev, and the licence table THIRD_PARTY.md
   already generates), and the legal note (no game data in this repo or artifact;
   you must own the game; not affiliated with Capcom). Keep the current dev README
   content by moving anything not already in `docs/` into it.
2. **Repo hygiene, verified while writing this plan**: 447 tracked files, all
   text/config — no game bytes, no `ppc/`, no captures (only the tracked
   `Xenia logs/Xenia_Run_Content.md` index). LICENSE (PolyForm Noncommercial 1.0.0)
   and THIRD_PARTY.md already at root. CI (`build.yml`) green on both legs and
   honest about what its tick means. The docs directory publishes as-is — it is
   written for outside readers by standing convention and is most of the project's
   value to other porters.

## §5 Publish

1. Push everything; confirm CI green at the release commit.
2. Tag (§1.1), write release notes: feature summary by era (boot → renderer → audio
   → save → 60 fps → KB/M → MSAA), requirements, known limitations, the §1.2 legal
   note, and the SHA-256 of each artifact.
3. Create the GitHub Release, attach both artifacts (per §1.2).
4. **Operator flips the repo public** (their account, their click — `gh` is not
   installed here and the visibility change should be a human act anyway).
5. After: watch the first issues; macOS stays milestone C (hardware-blocked); Case
   West inherits everything through `docs/reusability.md`.

## Order and effort

§0 is the only real code and comes first (one to two focused sessions; the encoder
port is the bulk). §1's answers can be gathered while §0 builds. §2-§3 are a session
including the czwin friction and one operator sitting. §4-§5 are a session. Nothing
else in the backlog blocks a v1.0: performance is parked by instruction with no lead
≥0.5 ms, RT is parked, the hair flicker is a documented known issue.

---

## §6 Execution record (2026-09-05, the session that ran §0-§4)

**§0 SHIPPED, and the identity gate passed on the first build.**
`runtime/host/overlay_gen.{h,cpp}` is the line-for-line C++ port of BOTH Python
generators — the real LZX verbatim-block encoder (greedy LZ77, package-merge
Huffman with the Python's exact tie-breaks, the pretree delta/run writer, the
five-byte readahead slack), per-chunk XMemCompress decode through XenonUtils'
own `lzxDecompress`, the options_pc.txt rewrite, the .bcs rebuilds, the
preload4 hash eviction, the layout.bin size pins, and the fecmn.tex glyph
patch with its three gates. The chip art ships pre-baked
(`gen_kbm_icons.py --export-chips` → `tools/release/kbm_chips/*.dxt`,
committed, 336 KB, our art only — PIL stays dev-only). Verified three ways,
each two-sided:

1. dev tree: Python outputs vs `cz_runtime --gen-overlays`, `diff -r` — zero
   differing bytes across all 14 files (and a flipped byte IS reported);
2. clean container: the new gate step [3/4] hashes the container-generated
   fecmn.big against the Python reference — matched, GATE PASSED;
3. staged bundle in a fake root: the AUTOMATIC boot path (no explicit verbs)
   extracted, generated, and produced overlays byte-identical to the
   reference.

A `.cz_overlay_version` stamp regenerates stale overlays on shipped updates;
`CZ_NO_OVERLAY_GEN=1` is the off switch (docs/instruments.md). The Python
tools carry the new contract in their docstrings: they are the REFERENCE and
the oracle; a transform change there must be ported + version-bumped in the
same commit.

**§2 DONE — both artifacts rebuilt at head and gated.**
* Linux: matched-configure Release + RelWithDebInfo arms both built;
  `release_text_identity.sh` OK (36,147,762-byte .text, identical);
  `release_package_linux.sh` (now stages kbm_chips/, all-25-or-refuse);
  `release_gate_clean_container.sh` GATE PASSED including the new overlay
  step. `CaseZeroRecomp-linux-x86_64.tar.zst` 26 MB, sha256 5561e071…
* Windows: czwin pulled to head; the predicted parts-87-96 MSVC friction was
  exactly ONE line — `memmem` in native_kbm.cpp's glyph scan, a GNU extension
  the Windows CRT lacks, replaced on both platforms with the same
  memchr-skip+memcmp helper (commit d125ec2) so the legs scan identically.
  overlay_gen.cpp needed /EHsc there (clang-cl defaults exceptions off; the
  flag is scoped to the one file). Build clean; `release_package_windows.ps1`
  staged, smoke-gated the staged exe, zipped.
  `CaseZeroRecomp-windows-x86_64.zip` 21 MB, sha256 a533e454…

**§3.3 ANSWERED BY MEASUREMENT — the pre-warm seed does NOT collapse under
MSAA 2x.** Structural reason first: `PipelineKey` carries no sample count —
pipelines take the renderer's live `msaaSamples` at creation, so the key file
is MSAA-agnostic by construction. Then the measurement: a clean second launch
of the staged bundle (fake root, fake HOME so the SHIPPED prewarm.keys is the
seed, MSAA 2x default active) created **757 of 1,365 — exactly the part-85
baseline**, the other 608 skipped for the documented vertex-shader-drift
reason. Beware the first attempt's two traps, recorded so nobody re-falls in:
session ONE always seeds ~0 (the vertex half of the cache is being born at
first sight — documented behaviour, not a collapse), and a dev-box run
without HOME redirected reads the DEV per-user key file, not the shipped
seed.

**§3.4 standing gates**: --smoke both platforms (staged exes), container
first-run flow end to end, text identity. Sync-validation and name-diff
unchanged by this session (no renderer or cache change).

**§4 DONE**: the public README is live at the root (players + porters + the
legal note); the day-1 dev README is preserved verbatim at
docs/dev-readme-day1.md with its retraction chain annotated. Repo hygiene
re-verified in §1's terms (no game bytes tracked).

**§5 prepared**: docs/release-notes-v1.0.0.md is the paste-ready Release
body, SHA-256 lines included (refresh them if §3's sitting forces a
rebuild). Tag NOT created yet — §3.1/§3.2 are §9.8's release blocker and
come first.

**Owed to the operator, in order:**
1. §3.1 — bundle save round-trip: Windows (czwin, `schtasks /run /tn
   cz_play` after staging) and Linux. Make a save, quit, relaunch, load.
2. §3.2 — same sitting: KB/M from the bundle (prompt icons now generated at
   first run — delete assets/game_kbm first to see it happen; the MASH A↔D
   struggle prompt is the newest visible), and the options screen from the
   bundle.
3. §5 — tag v1.0.0 at the verified commit, create the GitHub Release, paste
   docs/release-notes-v1.0.0.md, attach both artifacts, flip visibility.

**Addendum (same day, pre-§3 sitting): the MASH-on-pad fix.** The operator
caught the §0 design's one behavioural gap before testing: the kbm bank's
string edits are a boot-time choice, so a PAD player got the keyboard
wording (MASH on the struggle prompt) under correctly device-followed stick
art. Fixed in `74ab694`: the four keyboard wordings (PRESS ENTER ×2,
A / D KEYS, MASH) now ride the same device-follow worker as the glyph
texels — the loaded bank is located by its id-table prefix and each string
region swaps in place, verify-before-write both directions. Verified live
headlessly (synthetic presses are pad input): 4 strings swapped on the PAD
flip, and process_vm_readv of the running guest read 'LS ' at the id-4049
offset where the disk file holds 'MASH'. Both artifacts REBUILT at 74ab694
and re-gated (Linux: text identity + container GATE PASSED; Windows: staged
--smoke). Release-note SHAs refreshed: linux 59b8994d…, windows f0f97e56….

**Addendum 2 (same day): the always-on mouse.** Operator instruction: *"The
mouse should always be on so remove the settings in the visuals and set it
to always on."* Done in `46fab38`: the MOUSE CAMERA toggle is retired
(Visuals is seven rows; MOUSE SENS stays as the tuning knob), capture
follows keyboard focus alone, Settings_MouseCam is deleted and a stale
mouse_cam= line in an existing cz_settings.txt is an ignored unknown.
Both artifacts rebuilt at 46fab38 and re-gated (text identity + container
GATE PASSED; czwin staged --smoke OK). SHAs refreshed: linux efbd5cb9…,
windows c1c1eb13….
