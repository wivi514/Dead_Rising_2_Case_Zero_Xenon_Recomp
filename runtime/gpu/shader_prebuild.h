#pragma once
// Release D.3 — the first-run shader build: the disc's own pixel shaders, translated
// into the cache before the game starts.
//
// D.1 (docs/release-plan.md §9.1) established the split this module lives on: the disc
// holds **1,265 distinct pixel shaders, completely** — against the 345 accumulated over
// 25 parts and eleven operator sessions — and **zero usable vertex shaders** AS IS (the
// title patches vertex fetch instructions at bind time). Part 102 added the vertex pass:
// 102 of 104 runtime vertex shaders are a disc template plus a 2-32 dword patch, and
// `vs_recipes.bin` carries those patches (docs/part102-no-popin-plan.md). This pass reads `data/shaders/deadrisingprologue-ps.big`, decodes each
// `.po` object's microcode (D.1's container rule: start = u32@0x04 + u32@(u32@0x18),
// the tail of the object), and translates every shader the cache does not already hold
// through the same in-process pipeline the D.2 gate proved byte-identical to the
// offline one.
//
// RESUMABLE BY CONSTRUCTION, and self-detecting. Each shader is its own .spv+.meta.json
// pair, so an interrupted pass simply skips what it already wrote next time. Whether a
// pass is OWED is recorded in two marker files in the cache directory —
// `disc_prebuild.started` at pass begin, `disc_prebuild.done` at success — so a boot can
// tell "interrupted first run, finish it" from "a developer cache that was never built
// from the disc and must not be grown under the gates that count its 449 entries".
#include <filesystem>
#include <functional>

namespace ShaderPrebuild
{
// Translate every pixel shader in the disc bank that `cacheDir` does not already hold.
// Prints progress; returns 0 if every decodable object translated (failures are named
// and counted, never silently skipped). Creates `cacheDir` if needed.
// `progress(done, total)` fires only on the CALLING thread (between its own
// translations) — main.cpp feeds it to the first-run progress window, which is SDL
// and single-threaded by rule; the CLI passes nothing.
// Part 102: `vsBank` + `recipes` add the VERTEX pass — the disc's `.vo` templates with
// `vs_recipes.bin` (tools/vs_recipes.py) applied, each result hash-gated against the
// runtime shader it claims to be. Either path empty or missing = pixel-only, as before;
// CZ_NO_VS_RECIPES=1 is the same-binary control arm.
int BuildFromDisc(const std::filesystem::path& psBank,
                  const std::filesystem::path& cacheDir,
                  const std::function<void(unsigned, size_t)>& progress = {},
                  const std::filesystem::path& vsBank = {},
                  const std::filesystem::path& recipes = {});

// Whether a first-run pass should run at boot: the cache directory is missing or holds
// no modules (a player's first launch), or a `started` marker has no `done` beside it
// (an interrupted pass). A populated cache with neither marker is a developer cache and
// is left alone.
// A finished pixel pass with no vertex-pass marker (or a marker stamped with a different
// recipe file size) is owed the vertex pass, when `recipes` exists.
bool WantedAtBoot(const std::filesystem::path& cacheDir,
                  const std::filesystem::path& recipes = {});
} // namespace ShaderPrebuild
