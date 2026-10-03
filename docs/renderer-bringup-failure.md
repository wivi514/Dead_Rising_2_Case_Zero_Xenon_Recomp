# Renderer bring-up failure — imported from Case West issue #12 (2026-10-03)

Case West's player issue #12: a Ryzen 7 4800H laptop (Radeon iGPU), Windows 11, got
**audio but a white window**; the operator hears the same mostly from integrated-
graphics players (Intel and AMD). The F9 report read `gpu: unknown`, `vulkan: 0.0.0`,
which bounds the failure to `vkCreateInstance` or `vkEnumeratePhysicalDevices` — the
GPU line is filled only after both. Case Zero's bring-up is the same code line for
line, so the same machines fail here the same way. Full write-up:
`~/GithubRepo/Dead_Rising_2_Case_West_Xenon_Recomp/docs/player-issue-12.md`.

Two commits came across with the `CW_` -> `CZ_` rename, both applying by context:

1. **The failure says where** (CW `a25bb9a`). The first bring-up failure is kept in
   words; `system.txt` in an F9 report gains `renderer: up | FAILED: <step> | not
   started`; a message box names the step, the usual fix (the vendor's own driver,
   both GPUs on a laptop) and where `cz_runtime.log` is. `CZ_NO_RENDERER_DIALOG=1`
   suppresses the box.
2. **The implicit-layer retry** (CW `b90345b`) — a candidate fix. Only when instance
   creation fails or lists no device: retry with
   `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`, then `VK_LOADER_LAYERS_DISABLE=~implicit~`.
   The working rung prints `THIS RUNG WORKED`. `CZ_VK_NO_LAYER_RETRY=1` is the control.

## Positive control, re-run on this repo (Linux, RTX 3070)

A fake implicit layer (`VK_ADD_IMPLICIT_LAYER_PATH`, enabled by `FAIL_LAYER_ON=1`)
whose `vkCreateInstance` returns `VK_ERROR_INITIALIZATION_FAILED`. Its source is ten
lines: export a `vkGetInstanceProcAddr` that returns the failing function for
`"vkCreateInstance"`, and a manifest whose `disable_environment` picks the arm.

| arm | result |
|---|---|
| no fake layer | no retry line, `renderer UP` |
| layer disabled by the AMD variable | rung 1 WORKED, `renderer UP` |
| layer with an unrelated disable variable | rung 1 fails, rung 2 WORKED, `renderer UP` |
| `CZ_VK_NO_LAYER_RETRY=1` | `vkCreateInstance failed: VkResult -3`, no renderer |

## Owed

- The Windows compile on czwin (`_putenv_s` and the SDL message box are untested there).
- A report from a failing iGPU machine on a build carrying this: its log either says
  `THIS RUNG WORKED` (the hypothesis) or its `renderer: FAILED` line names the step.
- `cz_runtime --diag` has its own instance creation and does not have the ladder.
