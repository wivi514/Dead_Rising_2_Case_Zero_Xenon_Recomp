# Third-party components and licensing

**This repository is licensed [PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0)**
(`LICENSE`). Free to use, modify, fork and redistribute for any noncommercial
purpose; selling it needs a licence from the copyright holder. Recorded 2026-08-07,
alongside the sibling Fable II port, which took the same licence the same day.

**A GPLv3 declaration made earlier that day was reversed**, on the premise — checked
and found false — that this repository already contained UnleashedRecomp code. It
does not; see below. Neither declaration was ever pushed, and no code was ever
copied, so nothing attached.

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

**So the licence here is a CHOICE, not a remedy** — and PolyForm Noncommercial is
chosen for the same reason as in the sibling port: users get everything (use,
modify, fork, redistribute), while selling stays with the copyright holder so a
rights-holder arrangement remains possible.

**UnleashedRecomp's code must stay out.** Adopting it would make this a GPLv3
derivative and remove that option. It remains a structural reference only — which
has been sufficient to reach phase C12. If that ever changes, the licence cannot
stand as written and must be revisited BEFORE the first line is copied.

---

## Components

### Used, and part of this repository

| component | licence | status |
|---|---|---|
| **UnleashedRecomp** (hedge-dev) | **GPLv3** | **Architecture and findings only. No code copied, and none may be** — see above. Structural reference; port the method, write our own. |
| **plume** (the RHI UnleashedRecomp renders through) | **MIT — licence VERIFIED** (recorded in CLAUDE.md) | Not vendored. MIT, so it carries no copyleft obligation if adopted. |

### Used, and NOT part of this repository

| component | licence | relationship |
|---|---|---|
| **XenonRecomp** (hedge-dev) | **MIT** | The static recompiler. Universal fixes go upstream under MIT. |
| **XenosRecomp** (hedge-dev) | **MIT** | The Xenos shader translator. Same arrangement. |
| **XenonUtils `xbox.h`** guest structs | MIT | GPL-compatible; retain the MIT notice on anything derived from it. |
| **Fable II port** (`~/GithubRepo/Fable2XenonRecomp`) | **PolyForm Noncommercial 1.0.0** (2026-08-07) | Sibling port. Findings flow freely both ways — that exchange is facts, not code, and needs no licence step. |

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
