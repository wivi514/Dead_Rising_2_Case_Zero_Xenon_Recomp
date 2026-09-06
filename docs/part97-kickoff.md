# Part 97 kickoff — POST-RELEASE. v1.0.0 is PUBLISHED and the repo is PUBLIC.

**Status date: 2026-09-06.** This is the hand-off for whatever comes after the
release. Read `docs/release-github-plan.md` (§6, §7 and addenda 1-5 are the
execution record) before touching anything release-shaped.

## §0 Where the port is

**v1.0.0 IS LIVE: https://github.com/wivi514/Dead_Rising_2_Case_Zero_Xenon_Recomp**
— Release "Dead Rising 2: Case Zero — Native PC Recomp v1.0.0" at tag `v1.0.0`,
both artifacts attached, repo public since 2026-09-05. Verified from the outside:
both public downloads were pulled back over the internet and hash **byte-identical**
to the gated artifacts (linux `0d1aa15c…`, windows `d911ad17…`; binaries commit
407eb79). The operator captured a launch screenshot and posted to Reddit.

The launch-night play session's numbers, for reference (play_0905_2146.log):
**~80-85 fps at 3440x1440 in ~9,000-10,000-draw crowd scenes**, median 12 ms,
0.0-0.3% of frames above twice the median, one 44 ms worst-case in ~40 minutes.

## §0b What the release arc shipped (this session, 2026-09-05, in order)

1. **§0 of the plan — first-run overlay generation in C++** (`runtime/host/
   overlay_gen.{h,cpp}`): line-for-line port of gen_pc_options.py +
   gen_kbm_icons.py (the real LZX encoder included), byte-identical on the first
   build and verified three ways (dev diff -r, container hash, fake-root
   automatic boot). Chips ship pre-baked (`tools/release/kbm_chips/`, 26 blobs,
   our art); `CZ_NO_OVERLAY_GEN=1` off switch; `.cz_overlay_version` stamp
   (currently 2). THE DISCIPLINE: any transform/chip change re-exports chips AND
   bumps `kGeneratorVersion` in the same commit — both Python docstrings state it.
2. **Both artifacts rebuilt + gated at head**; the whole predicted czwin friction
   of parts 87-96 was ONE `memmem` (now a portable FindBytes on both platforms).
3. **§3.3 answered by measurement**: the pre-warm seed does NOT collapse under
   MSAA 2x — 757 of 1,365, exactly the part-85 baseline; `PipelineKey` carries no
   sample count so the seed is MSAA-agnostic by construction.
4. **Operator fix round** (each operator-reported, same day):
   - **Device-following prompt WORDING** — MASH/PRESS ENTER/A-D KEYS swap in
     guest memory with the glyph art (the pad was showing MASH); verified live
     by process_vm_readv. The string bank is located by its id-table prefix,
     every write verify-before-write.
   - **Always-on mouse camera** — the Visuals MOUSE CAMERA toggle retired
     (panel is 7 rows), capture follows keyboard focus alone, stale `mouse_cam=`
     keys in old settings files are ignored unknowns.
   - **Case West back-imports** (their part-8 fixes on top of importing our
     stack): XMA hardware loops (CZ hits the path — ~5,900 sustains a run,
     `CZ_XMA_NO_LOOP=1` the arm), LRU texture-slot recycling (validated at cap
     256 under sync validation: 215 recycles, 215 paired destroys, 0 hazards;
     `CZ_VK_NO_TEX_LRU=1` the arm), and the Q legend for `y_button_ig` (our map
     binds the in-game Y actions to KEY_Q; the "no keyboard equivalent" note was
     stale). NOT taken: their epilogue-camera bindings and generator table
     addresses (CW-only), and everything already ours.
5. **Player-first documentation**: root README and the bundle README rewritten
   for players (install steps, controls, status: "essentially complete, 100%
   playable, looks right in nearly all places"); release notes body matching;
   both artifacts repackaged once for the bundle README (binaries unchanged).
6. **Published**: tag, Release, visibility — the operator's clicks. §7 of the
   plan is the closing record.
7. **Post-release README adds**: level cap 50 (vs the original 5, all fifteen
   skills), 21:9 ultrawide with the FOV ≥ +10 recommendation (stock FOV
   stretches Chuck ultrawide), Support section + `.github/FUNDING.yml`
   (github.com/sponsors/wivi514, verified live).

## §1 The standing state — what a next session picks from, in order

1. **Watch the public issues.** First-responder kit: the bundle README's
   bisection table; for picture complaints the standing order is
   `CZ_VK_NO_DEFERRED_CLEAR=1` → `CZ_VK_DEFER_FULL_RECT=1` →
   `CZ_VK_NO_PAR_RECORD=1`, now joined by `CZ_VK_NO_TEX_LRU=1` (new in this
   release) and `CZ_VK_NO_BIND_BATCH=1`/`CZ_VK_NO_DEVICE_PFN=1`.
2. **Repo topics are still unset** — operator's click; suggestions in plan §7.
3. **The glibc floor / AppImage** (release-plan E.2) — the one shipped
   limitation with a known fix path.
4. **macOS** — milestone C, hardware-blocked, ARM64 risk retired.
5. **Case West** — the next port, already importing from us (its part 8 took
   parts 83-93 wholesale). Everything new here transfers: the overlay-gen road,
   the release packaging + gate ladder, the string-follow, the LRU, the loops.
   `docs/reusability.md` governs what gets extracted (proven in BOTH ports).
6. **Parked, unchanged**: performance (no lead ≥0.5 ms either side), RT shadows
   (`CZ_VK_RT_MENU=1` restores the rows), the hair flicker
   (`docs/hair-flicker-part92.md`, OPEN, localized to a shader class).

## §2 Rules that now bind (learned or hardened this arc)

- **The release is FROZEN at the tag.** Never rebuild an artifact casually: a
  rebuild means refreshing the SHAs in `docs/release-notes-v1.0.0.md` FIRST,
  re-gating, and — now that the Release is public — updating the published
  Release's assets and notes, not just the tag.
- **The Python generators are the ORACLE, the runtime is the road**
  (stfs_extract precedent, now overlay_gen too). A transform change ports to
  C++ + bumps the version in the same commit; the container gate hashes the
  container's output against the dev tree's Python output every run.
- **Both release stages are play copies.** Cleaning one back to the skeleton:
  check where the operator's only package copy is FIRST (the czwin dev tree
  holds theirs at `C:\cz\...\assets\package\`; the Linux dev tree at
  `assets/package/`).
- The operator's capture shortcut is the OS snipper, not F9 — remind them
  Spectacle deletes its temp dir (a capture that isn't SAVED is lost; the
  INDEX.md scar).
