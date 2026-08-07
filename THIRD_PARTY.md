# Third-party components and licensing

**This repository is licensed GPLv3** (`COPYING`). Recorded 2026-08-07, at the
operator's instruction, alongside the sibling Fable II port which took the same
licence on the same day.

`docs/d3d-translation-plan.md` recorded the decision to adapt UnleashedRecomp's code
("get the maximum") before this file existed. CLAUDE.md §"Reuse strategy" requires
the licence of every borrowed component to be recorded **before the first line is
copied**; this file is that record, and it is created before any copying.

---

## What is actually in this tree today — checked, not assumed

**As of 2026-08-07 this repository contains NO UnleashedRecomp code.** Verified by
inspection: zero occurrences of "unleashed" anywhere under `runtime/`, zero
provenance headers, and the D3D layer is original work —

* `runtime/gpu/d3d_hooks.cpp` hooks the title's own statically-linked XDK functions
  through XenonRecomp's `__imp__sub_X` weak-alias seam. Its own design.
* `runtime/gpu/d3d_draw.cpp` is, in its own words, *"a faithful subset of
  gpu/pm4.cpp's packet decode"* — derived from **this repository's** PM4 executor,
  not from anyone else's renderer.

So the D3D translation layer here was built from UnleashedRecomp's **architecture**
(hook at the D3D line; `VdSwap`/`VdInitializeRingBuffer` become dead code) and from
this port's own PM4 knowledge. Architecture and findings are not copyrightable;
**no licence obligation has been incurred by that.**

**The licence is therefore a CHOICE here, not a remedy.** It is taken deliberately:
to keep this port in the same commons as the tools it is built on, to stay
consistent with the Fable II port, and because the plan reserves the right to adapt
`video.cpp` directly later — at which point GPLv3 stops being optional.

**If that changes, this section must change with it.** The moment a file is adapted
from UnleashedRecomp, it gets a provenance header and a row in the log below.

---

## Components

### Used, and part of this repository

| component | licence | status |
|---|---|---|
| **UnleashedRecomp** (hedge-dev) | **GPLv3** | **Architecture and findings only. No code copied as of 2026-08-07.** Approved for direct adaptation when wanted; every adapted file must carry a provenance header and a row here. |
| **plume** (the RHI UnleashedRecomp renders through) | **MIT — licence VERIFIED** (recorded in CLAUDE.md) | Not vendored. MIT, so it carries no copyleft obligation if adopted. |

### Used, and NOT part of this repository

| component | licence | relationship |
|---|---|---|
| **XenonRecomp** (hedge-dev) | **MIT** | The static recompiler. Universal fixes go upstream under MIT. |
| **XenosRecomp** (hedge-dev) | **MIT** | The Xenos shader translator. Same arrangement. |
| **XenonUtils `xbox.h`** guest structs | MIT | GPL-compatible; retain the MIT notice on anything derived from it. |
| **Fable II port** (`~/GithubRepo/Fable2XenonRecomp`) | **GPLv3** (2026-08-07) | Sibling port. Findings flow freely both ways — that exchange is facts, not code, and needs no licence step. |

### Not distributed, and not licensable by us

| item | note |
|---|---|
| `assets/` | Retail *Dead Rising 2: Case Zero* data. Copyrighted by its owners. Gitignored; never distributed. |
| `ppc/` | Generated C++ recompiled from the retail executable — a derivative of copyrighted material. Gitignored; regenerated locally from a legally-obtained copy. |

---

## Rules for adapting code from here on

1. **Provenance header on every adapted file**, naming the upstream project, the file
   it derives from, and its licence. 90 % ours and 10 % theirs still needs it.
2. **Record the component above before the first line is copied.** Adding it
   afterwards defeats the rule.
3. **Keep the MIT components separable.** Fixes to XenonRecomp/XenosRecomp belong
   upstream in those repos under MIT, not vendored here under GPLv3.
4. **Facts are free; expression is not.** Porting a *method* from a sibling port's
   docs needs no licence step. Copying its `.cpp` does.
