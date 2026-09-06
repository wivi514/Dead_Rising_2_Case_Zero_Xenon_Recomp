# Localization + boot-logo skip — the two fixes gating v1.0.1 (part 99 plan)

Operator instruction (2026-09-06): *"need to do a few more fix before we release
1.0.1 … make a plan to fix localization so we can select all subtitle that are in
the game from the launcher. And the other thing … a toggle in the launcher to skip
boot logo (Capcom, blue castle game and dolby digital)."* Part 98 (async pipelines)
is already in-tree; these two join it and then v1.0.1 ships all three
(`open-items.md` 0x).

## §0 Recon already done (this session — every claim below was measured tonight)

**Localization.**
- The game ships **six real subtitle languages**: `data/frontend/str_{en,fr,it,es,ja,ko}.bcs`
  — eight files, all 441,496 B (fixed-slot layout, which is why the KB/M string work
  was size-pinned before the rebuild tool), all with distinct content (332-362 KB of
  differing bytes vs en). `str_id.bcs` is the **label-ID table** (IDS_* names), and
  `str_lg.bcs` is a **mixed-language legal/pre-language table** (French AND Spanish
  lines in one bank) — neither is a selectable language. **There is no German.**
- The guest builds the filename from a data-driven suffix via the format string
  `%s/str_%s%s` (0x820B7838) and a suffix table at **0x82071288**:
  `ko ja lg es it fr en` (ascending). The index→suffix mapping is NOT yet derived —
  see §1 step 1, which gets it by experiment rather than arithmetic.
- Our HLE hardcodes English in TWO places (`runtime/kernel/imports.cpp`):
  `ExGetXConfigSetting_x` category 3 setting 9 → 1, and `XGetLanguage_x()` → 1.
  A1 shows the title asking setting 9 exactly once, at boot — so a language chosen
  in the launcher (which runs before the guest) is live the same run, no
  mid-session apply needed.
- CJK is plausibly renderable out of the box: `data/system/{480,720}/arialko.bcf`
  and `arialutf.bcf` ship (the `%d` in `data/system/%d/%s.bcf` is the RESOLUTION,
  not the language). Whether ja/ko actually draw complete glyphs is an eye
  question — §1 step 4.
- **The one real interaction: `overlay_gen.cpp`'s KB/M layer edits `str_en.bcs`
  ONLY** (PRESS START→PRESS ENTER, LEFT STICK→A/D KEYS, the id-4049 LS→MASH
  rebuild). The part-60 options layer already enumerates ALL `str_*.bcs` banks
  (`StrBankNames`), so the options screen is fine in every language — but a
  non-English player gets the PAD wording back on the title screen and the
  struggle prompt. §1 step 5 decides how much of that to close.

**Boot logos.**
- The logos are not frontend screens — `path_fe.txt` starts at `PressStart`.
  They are three of the game's TOP-LEVEL STATES, registered by interned name at
  0x829A21C0 into a global table at **0x82A6912C**:
  `+4 Startup, +8 LegalScreen, +0xC BCGIntro, +0x10 FrontEnd, +0x14 FEToGame`,
  then `Loading`, `InGame`, … (names at 0x82072DC0..; intern function 0x8276E398,
  the same one the DebugJump screen-request machinery already drives).
  `cLegalScreenFlow` (0x8208B968) is resolved by a strcmp factory at 0x825BC3C0.
- `data/capcom.txt` is opened by the image (0x8278AB94) and ABSENT from the
  package — a dev-config candidate worth one look (§2 step 1), but the plan does
  not depend on it.
- **The "stuck on Capcom logo" report may already be part 98's bug**: on a weak
  machine, session one compiles every pipeline synchronously during exactly these
  screens (our own arm S measured 3.7 s inside ONE frame on a fast box — a slow
  iGPU turns that into minutes of apparent hang, on the logo). §2 step 0 asks the
  reporter before we treat the toggle as the fix. The toggle is worth shipping
  either way.

## §1 Fix 1 — subtitle language from the launcher

1. **Derive the ID→suffix mapping by experiment, not by reading the table.** Add
   `CZ_LANGUAGE=N` (dev arm, wins over the setting): both HLE sites return N. Boot
   with `CZ_FILE_TRACE=1` for N=1..8 and record which `str_XX.bcs` opens. Xbox IDs
   are 1=en 2=ja 3=de 4=fr 5=es 6=it 7=ko 8=zh; the interesting rows are 3 and 8
   (unsupported — expect a fallback to en or lg, and whatever it is gets written
   down, because the launcher must only offer the six that work). Prediction:
   each supported N opens exactly its own bank, once, at boot.
2. **Settings + launcher row.** New persisted setting `language` (store the Xbox
   ID, default 1) through the same `Settings_*` API both UIs share; launcher row
   `SUBTITLES: ENGLISH / FRANCAIS / ITALIANO / ESPANOL / JAPANESE / KOREAN`
   (ASCII names — the launcher's 5x7 glyph font has no accents or CJK; the
   in-GAME text is what gets localized). The two HLE sites read the setting.
   Order the row list by the measured mapping, never by assumption.
3. **Do NOT add the row to the in-game F4 panel in this part.** The language is
   read once at boot (measured, A1); a live row would need a reload mechanism the
   title doesn't offer. The launcher is the right home — it runs pre-boot. If the
   operator wants it visible in-game later, it shows the value read-only.
4. **The eye pass (operator or frame dump):** one boot per language to the title
   screen + one subtitled cinematic (the prologue intro is subtitled and 30 s in).
   Checks: menu text in-language, subtitles in-language, ja/ko glyphs actually
   render (the shipped arialutf/arialko make it likely, not proven), and the
   part-60/KB overlay options screen still shows its added rows (it should — all
   banks are patched by that layer).
5. **The KB/M wording interaction, decided small:** extend ONLY the id-keyed
   edit (id 4049 LS→MASH, a table rebuild, language-independent mechanics) to all
   six banks in `overlay_gen.cpp`; leave the English-literal edits (PRESS
   START/LEFT STICK) en-only and record that a non-English player sees the pad
   wording for those two — translating them is a content decision the operator
   owns, not a code fix. Bump `kGeneratorVersion` (the overlay contract requires
   it in the same commit).
6. **Gates:** the byte-identity gate between C++ overlay_gen and the Python
   reference (it enumerates banks already — the id-4049 change must land in BOTH
   and stay byte-identical); `CZ_FILE_TRACE` shows exactly one `str_XX.bcs` open
   matching the setting; A5 kernel-diff unchanged (the setting changes a VALUE,
   not the call sequence); all six languages boot to the title headlessly.

## §2 Fix 2 — skip the boot logos (launcher toggle)

0. **First, the report itself:** reply to / test with the part-98 build and ask
   for the boot log (`[vk] pipeline pre-warm:` + the creation census say it
   instantly). If their hang is session-one pipeline compilation, the fix is
   already in-tree and the toggle is a comfort feature, not the bug fix. Do not
   close their report against the toggle without the log.
1. **Recon the transition (one sitting):** find the reader of the state table's
   `+8`/`+0xC` slots (the code that requests `LegalScreen` then `BCGIntro`), via
   `gdis.py --find-uses` on 0x82A69134/0x82A69138 and a `CZ_GUEST_DIAG` boot for
   the state machine's own prints. Also spend ten minutes on `data/capcom.txt`
   (0x8278AB94): if it is a shipped dev-config reader with a skip flag, the whole
   fix might be one generated file in the overlay — cheaper and more faithful
   than a hook. Decide (a) vs (b) below on what the code shows.
2. **The mechanism, in preference order:**
   (a) **Substitute the transition target**: a PPC hook on the state-request
   function — when the toggle is on and the requested state id is
   interned("LegalScreen") or interned("BCGIntro"), request
   interned("FrontEnd") instead. One hook, two names, and the states'
   enter/exit code never runs — which is also its RISK: logo states routinely
   mask async warm-up (streaming init, the FE preload). If boot races appear,
   fall back to:
   (b) **Shorten, don't bypass**: hook the legal/intro flow's update to signal
   completion on its first frame — every enter/exit side effect still runs, the
   screens just last one frame. Slower to build (needs the flow object's
   layout), immune to the masked-load class.
3. **Settings + launcher row:** `skip_intro_logos`, default **OFF** — these are
   legal notices; shipping the skip as opt-in is the defensible default and the
   operator can revisit. `CZ_SKIP_INTRO=1` is the headless/dev arm. The runtime
   prints one line when the skip engages (the assert-that-an-arm-engaged rule).
4. **Gates:** with the toggle ON: headless boot reaches PressStart (the
   `CZ_FAKE_START_MS` recipes must still work with their timings — they are
   fixed-interval and a faster boot SHIFTS them, so re-derive the recipe's
   arrival or key the gate on the WAITJUMP form, which is event-anchored);
   deepest-file gate unchanged or deeper; A5 diff run once (expect file-order
   changes in the logo era — pre-register that as acceptable, everything else
   not). With the toggle OFF: byte-for-byte the current boot (the hook must not
   exist on the path at all — same shape as every other arm).
5. **Prediction to falsify:** toggle ON saves 10-20 s of wall time to the title
   screen and produces zero new `[kcall]`/file divergences outside the logo era.
   If skipping exposes a black screen or a hang at FrontEnd, mechanism (a) is
   refuted for this title and (b) is the plan.

## §3 Order and the release

Fix 1 first (bounded, no guest-code risk, all recon done), fix 2 second (one
unknown left — the transition site), then the v1.0.1 flow from
`part98-kickoff.md` §1 with all three changes in one release. The operator's
new-PC session-one test remains gate 0 and can double as the eye pass for both
fixes (pick a language, toggle the skip, feel the first boot).

## §4 Out of scope, said out loud

Audio localization (the game ships English VO only — nothing to select), German
or any language the disc does not carry, translating the two English-literal KB/M
title-screen edits, an in-game live language switch, and making the skip toggle
default-on.
