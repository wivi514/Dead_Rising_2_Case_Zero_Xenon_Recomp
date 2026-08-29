#pragma once
// Release D.3 — the first-run shader build: the disc's own pixel shaders, translated
// into the cache before the game starts.
//
// D.1 (docs/release-plan.md §9.1) established the split this module lives on: the disc
// holds **1,265 distinct pixel shaders, completely** — against the 345 accumulated over
// 25 parts and eleven operator sessions — and **zero usable vertex shaders** (the title
// patches vertex fetch instructions at load, so those come from D.4's first-sight path
// at run time). This pass reads `data/shaders/deadrisingprologue-ps.big`, decodes each
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

namespace ShaderPrebuild
{
// Translate every pixel shader in the disc bank that `cacheDir` does not already hold.
// Prints progress; returns 0 if every decodable object translated (failures are named
// and counted, never silently skipped). Creates `cacheDir` if needed.
int BuildFromDisc(const std::filesystem::path& psBank,
                  const std::filesystem::path& cacheDir);

// Whether a first-run pass should run at boot: the cache directory is missing or holds
// no modules (a player's first launch), or a `started` marker has no `done` beside it
// (an interrupted pass). A populated cache with neither marker is a developer cache and
// is left alone.
bool WantedAtBoot(const std::filesystem::path& cacheDir);
} // namespace ShaderPrebuild
