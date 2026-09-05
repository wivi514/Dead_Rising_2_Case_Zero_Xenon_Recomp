# Dead_Rising_2_Case_Zero_Xenon_Recomp

A **XenonRecomp**-based static recompilation of the Xbox 360 XBLA title
**Dead Rising 2: Case Zero** (Capcom / Blue Castle Games, 2010).

This is the **third** game ported with this pipeline in this workspace. The first two are
the playbook and should be read before re-deriving anything:

- `~/GithubRepo/Fable2XenonRecomp` — the original, and the deepest: 91k functions to a
  live rendered world, with the whole methodology in its `CLAUDE.md` and `docs/`.
- `~/GithubRepo/Asuras_Wrath_Xenon_Recomp` — the second, which proved the template
  transfers and added a numbered findings ledger plus a set of transferable gotchas.

Dead Rising 2: Case West is planned next; it is the same engine and most of what is
learned here should carry over directly.

## Why this title is different from the first two

Both template ports were **disc** titles: an XGD ISO that `extract-xiso` opens in one
command, with a retail-key, basic-compression XEX. Case Zero is an **XBLA** title, and
every one of those assumptions is false:

| | Fable 2 / Asura's Wrath | Case Zero |
|---|---|---|
| container | XGD ISO | STFS package (`LIVE`, arcade title) |
| unpack | `extract-xiso` | `tools/extract_stfs.py` (written here) |
| XEX encryption | retail key | **devkit (all-zero) key** |
| XEX compression | basic (block table) | **normal (LZX)** |
| offline image | `tools/decrypt_xex.py` | `tools/xex_image_dump` (uses XenonRecomp's own loader) |

The devkit key is the one that actually blocks you: stock XenonRecomp hardcodes the
retail key and returns an **empty image with no diagnostic** when it is wrong. See
`docs/xenonrecomp-upstream-bugs.md`.

## Status

- [x] STFS package unpacked — 256 files, 825 MB, `default.xex` + `data/`.
- [x] XEX identity established: title `58410A8D`, base `0x82000000`, entry `0x825D9F30`,
      image `0xB40000`, `.text 0x82150000 + 0x873564`.
- [x] Save/restore helper ladders located and cross-checked (`tools/find_save_restore.py`).
- [x] **232 jump tables recovered** (105 absolute, 85 offset8, 42 offset16, 6,114 case
      labels) — XenonAnalyse finds **zero** on this binary.
- [x] **Recompilation succeeds and the recompiler log is completely silent:
      57,822 functions → 156 MB of C++ in `ppc/`** — zero unrecognized instructions, zero
      undecodable instructions, zero switch-boundary errors, zero dropped branches.
- [x] **Xenia ground truth round 1 complete** — A1–A5, B1/B1b/B2, C1/C2, D, E, all as the
      full game. Analysis: `docs/xenia-capture-analysis.md`.
- [x] **Forwards coverage oracle applied** — 110 entry points hardware executed that
      static analysis never found.
- [x] **`.big` container format cracked** — `docs/big-archive-format.md`.
- [x] **42 unrecognized-instruction sites (6 mnemonics) implemented in XenonRecomp** —
      plus a 7th, `vadduws`, which was already "implemented" against a simde intrinsic
      that does not exist and could never have compiled.
- [x] **Dropped direct branches driven to zero** — a defect class nothing here was
      measuring, because the check for it grepped a pattern the recompiler never emits.
      `tools/find_dropped_branches.py`, findings 13 and 5d.
- [x] **The image compiles and links — phase 0 complete.** All 228 TUs build with
      **0 errors and 0 warnings** (89 s on 16 cores) into a 155 MB `libppc_image.a`;
      `runtime/build/cz_smoke` links every generated symbol (`--whole-archive`) and
      validates all 58,303 `PPCFuncMappings` entries. Zero undefined symbols.
- [x] **`.xtr` GPU-stream decoder written, and the determinism baseline measured.**
      B1 and B1b are content-deterministic to **0.42%** over the boot+movie prefix
      (0.19% on draws), with four eras agreeing to the individual draw — but only
      **80.0% frame-exact**, so phase 4 must gate on per-era aggregates rather than on
      frame index. Both captures intact, zero desyncs, all 21 type-3 opcodes named.
      `docs/xtr-decoder.md`, finding 10.
- [ ] Runtime (kernel HLE, GPU command processor, renderer, audio) — `docs/runtime-plan.md`.

## Game intel

Blue Castle Games' in-house engine, shared with the full Dead Rising 2 (the image still
carries DR2's zone names — americana, atlantica, arena_stadium — though Case Zero ships
only the Still Creek content). Middleware: **Havok** physics, `.big` archive containers,
an in-house "CrowdEngine" for the zombie crowds, XMA audio — and, unlike both template
ports, **no Bink**.
244 kernel/XAM imports.

~~Notably, **the shaders ship as loose banks on disc**
(`data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big`) rather than embedded in
packages, which should shorten the path to XenosRecomp considerably compared with
Fable 2's `.sbk` extraction work.~~

**Retracted** (analysis finding 6): those banks are `.big` archives of `<hash>.vo` shader
*objects* carrying build metadata (including `.updb` debug paths), not the microcode the
guest submits — their payloads share only background-noise n-gram overlap with the real
thing. Middleware or asset names in an image tell you a name exists, not that a format is
in use. The renderer's input comes instead from Xenia's `dump_shaders`: **455 raw Xenos
microcode blobs**, already captured, which is what XenosRecomp consumes.

## Regenerate the C++

```
cd config && ~/GithubRepo/XenonRecomp/build/XenonRecomp/XenonRecomp CaseZero.toml \
    ~/GithubRepo/XenonRecomp/XenonUtils/ppc_context.h
```

## Build it

**Clang is required** — `ppc_context.h` uses `__builtin_assume`, which GCC has no
spelling for, so GCC fails in every one of the 57,822 generated function bodies.

```
python3 tools/gen_import_stubs.py
cmake -S runtime -B runtime/build -G Ninja
cmake --build runtime/build -j$(nproc)
./runtime/build/cz_smoke
```

`assets/` is not committed (copyrighted game data). Recreate it with:

```
python3 tools/extract_stfs.py "<the XBLA package>" -o assets/game
./tools/build_xex_image_dump.sh && ./tools/xex_image_dump assets/game/default.xex \
    assets/game/default_image.bin
```

## Register save/restore addresses (Case Zero XEX)

```
savegprlr_14=0x8280FED0  restgprlr_14=0x8280FF20
savefpr_14  =0x82810000  restfpr_14  =0x8281004C
savevmx_14  =0x82815190  savevmx_64  =0x82815224
restvmx_14  =0x82815428  restvmx_64  =0x828154BC
```

---

*This file was the repository's root README from day 1 until the public release
(2026-09-05, release-github-plan §4). It is kept verbatim as the day-1 snapshot —
including its own in-place retraction of finding 6, which was later itself
partially re-retracted (the disc banks DO carry pixel-shader microcode; see
`xenia-capture-analysis.md` §6 and the CLAUDE.md shader-bank section). The public
README that replaced it is written for players and outside porters.*
