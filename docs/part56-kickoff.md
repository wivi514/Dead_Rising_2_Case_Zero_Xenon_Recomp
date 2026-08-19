# Part 56 kickoff — performance is PARKED; the subject is the last few visual bugs

> **THE OPERATOR SET THIS SUBJECT** at the close of part 55: *"Save all of what is needed
> for performance later on and all your finding. We'll switch to fixing the last few visual
> bugs for the next few sessions and we'll come back to performance later."*
>
> **Performance is parked, not abandoned, and it is parked in ONE document:
> `docs/perf-state-parked.md`.** It carries where the frame is, the pump thread's current
> symbol table, the four remaining items in order with their risks, the three ways part 55
> got its measurements wrong, every arm that exists, and the four things owed to whoever
> picks it up. **Do not re-derive any of it and do not read `perf-plan-part55.md` as live** —
> its §0 and §0b were executed, the rest is superseded.
>
> Read this file, then `docs/open-items.md` for the item you pick, then
> `phase5-notes.md` §6cl for what part 55 changed under the renderer.

Written at the close of part 55 (2026-08-18). **This is the LIVE hand-off**, superseding
`part55-kickoff.md`.

---

## 1. WHAT PART 55 LEFT UNDER THE RENDERER, because it touched code the picture depends on

Five lookup tables became flat open-addressed tables, and the ALU constant copy is now
memoised per constant window. Both are correctness-critical paths — a wrong lookup serves
another mesh's vertices or another draw's transform matrix — so **both ship with a control
arm and a verified checker**, and if a picture defect is ever suspected of being part 55's
doing, these are the two switches that answer it in one run each:

```
CZ_VK_NO_FLAT_CACHE=1     std::unordered_map / std::map, i.e. the pre-part-55 renderer
CZ_VK_NO_CONST_MEMO=1     all 8 KB of ALU constants copied every draw, as before
```

Both verifiers read zero over millions of lookups and both poison arms fire, so neither is
a likely suspect — but they cost one run to eliminate, which is cheaper than reasoning.

**Nothing else about the picture changed in part 55.** No shader, no format decode, no bind,
no pass. The gates were re-run whole at the close (§5).

---

## 2. THE VISUAL ITEMS, and the two the operator named themselves

They are all in `docs/open-items.md` with their evidence. In the order that respects what is
known rather than what is annoying:

### 00m — DECALS. Operator-reported, never investigated, no captures.
*"still has the issue with decals that I think I didn't warn you about"* — a standing defect
that was never filed, reported in part 47 alongside the confirmation that the performance
work had not changed the picture. **It is theirs to characterise**: nobody here has seen it,
and the first action is to ask what it looks like, not to start measuring. The likely handle
once it is described: decals are a separate pass with their own blend state, so
`CZ_VK_DRAW_CENSUS` on a frame containing one plus `CZ_VK_DRAW_ID` should name the draws in
a single capture. **Check the title screen and menu backdrop first for a self-servable
repro** (gotcha 319) — a defect with an oracle you can run yourself is worth ten that need a
play session.

### 00n — A SIGN AND SOME ITEMS WRONG AT DISTANCE.
*"some sign and item that still got issue with distance but this is not introduced by your
performance fix."* The tail of 00i's flat-at-range class, most of which part 45's
interpolant-liveness fix closed on the operator's own A/B. **The first action is a
measurement that already exists and has not been re-run**: the parked mip-selection
overshoot's decisive arm, `CZ_VK_NO_MIPS=1` on the FIXED shader cache. That is not a new
investigation, it is an owed one.

### 00f — WHITE PATCHES ON WORLD SURFACES. At least two defects, seven explanations refuted.
The longest-standing picture item. **Do not re-buy any of the seven** — the tone map, a
missing texture, constant UVs, the white dummy, the clear colour, the EDRAM surface format,
a flat-decoding texture are all measured dead. Part 27 closed the last input and put the
ground defect **in the shading**; the named next step is reading our translated
`ps_ad65b98593f95926` against the capture's own disassembly of it. That step has never been
taken.

### 00d — TWO VULKAN VALIDATION DEFECTS REMAIN.
20 `VkGraphicsPipelineCreateInfo-Input-08733` and 6 `...topology-08773` on the outdoor
route, and nothing else. Quote that as the standing gate. Cheap, self-servable, and this
project has had one validation run name a picture defect in minutes before.

### Also open, from CLAUDE.md's standing list
the shadow cascade, NPC part meshes, mipmaps, and the colour-grading LUT — each with its
measurement in `open-items.md`.

---

## 3. WHAT MAKES A PICTURE SESSION WORK HERE, in one place

* **Ask the oracle first.** *"Does Xenia show this too?"* is one line to the operator and
  can retire a whole day. Every one of the captures in `Xenia logs/` is hardware's answer to
  a question somebody has already asked.
* **A matched-frame picture A/B is unsatisfiable outdoors** — 0 of 12,174 frames match
  between two runs of ONE configuration, because a crowd never renders the same draw list
  twice. Use `tools/frame_era_medians.py` (era medians over frames above 1,800 draws, null
  measured from the same pair: 0.94% on mean luma, 0.76% on distinct colours).
* **Prefer a repro that already has an oracle**: the title backdrop and the menus are static,
  self-servable, and capture E3 is a photograph of one of them.
* **Name the property a fix should move before running it.** "Does it still look wrong"
  cannot tell a 40%-fixed defect from a 0%-fixed one.
* **One frame of an animated scene is ONE SAMPLE** (gotchas 133, 268) — the E3 gate itself
  had to be repaired into a best-of-five for exactly this.
* **Census the shader bank before theorising** — parsing the 439 `.spv`/`.meta.json` beat
  three rounds of reasoning in part 23.
* **A saturated count measures its emitter**, not the population (gotcha: 324/324 of our
  shaders "discard" because XenosRecomp emits the clip unconditionally; hardware's microcode
  says 1 of 208).

---

## 4. STANDING STATE at the close of part 55

* **Runtime defaults**: 500 fps cap (1 ms vblank period), host vsync off, 100 us ring tick,
  swapchain present, internal resolution 1280x720, **flat containers ON**, **constant memo
  ON**, geometry in system RAM, guard pool at whatever the thread budget grants (3 on an
  8-physical-core machine).
* **Every run now prints its own configuration**: `[threads] machine: ... -> budget N
  workers`, each big buffer's memory placement, `[vk] const memo: ...%`, `[vk] flat cache
  grows: ...`. Quote those with any number.
* **New tooling**: `tools/part55_srcline.py` (split a symbol by source line out of a `perf`
  profile, free), `tools/part55_chained_ab.sh` (the operator's two-soak harness, arms are one
  env var each), `tools/part55_item_campaign.sh` (the roaming campaign, generalised —
  **and gotcha 355 says prefer the soak**).
* **Artifacts**: `~/DR2CZ-troubleshooting/part55/` (symbol profiles, both roaming campaigns)
  and `~/DR2CZ-troubleshooting/part55-operator/` (four chained soak sessions).

---

## 5. GATES AT THE CLOSE OF PART 55

Re-run whole after the container work, the constant memo and the VRAM arm — see §5 of the
status block in `CLAUDE.md` for the numbers. **The two rows part 55 had to invent** are the
flat-cache verifier (0 of 48.5 M lookups disagreed, poison 81.7%) and the constant-memo
verifier (0 of 117,521 and 0 of 119,019, poison 100.0000%); both belong in any future close
that touches those paths.
