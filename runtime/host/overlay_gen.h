// The first-run overlay generator (release-github-plan §0).
//
// WHY THIS EXISTS. The patched-asset overlays — assets/game_patched (the resurrected
// PC options screen, PRESS ENTER, MASH and every string edit since part 60) and
// assets/game_kbm (all 25 keyboard prompt icons + the device-follow sidecar) — are
// generated in the dev tree by tools/gen_pc_options.py and tools/gen_kbm_icons.py
// FROM the game data. They carry Capcom-derived bytes (repacked banks are mostly
// Capcom's own entries), so they can neither ship in the release artifact nor be
// regenerated on a player's machine that has no Python: without this module a
// shipped bundle silently loses the options screen and shows pad art on every
// prompt (kernel/vfs.cpp falls through to the shipped files without a word — the
// gotcha-5 shape).
//
// So the transforms live HERE, in the first-run flow, on the road host/stfs_extract
// proved in part 85: an in-process C++ port whose output is BYTE-IDENTICAL to the
// Python reference, which stays the dev tool and the oracle. The one asset the
// transforms need that is OURS and not derivable — the 25 key-cap chip images —
// ships pre-baked as raw DXT5 texel blobs (tools/release/kbm_chips/*.dxt, exported
// by gen_kbm_icons.py --export-chips; no Capcom byte, and PIL stays dev-only).
//
// The identity gate is free and exact: run the Python tools, run
// `cz_runtime --gen-overlays`, diff the trees. It is what makes this a port rather
// than a reimplementation, and it is run in CI's clean-container gate.
//
// CZ_NO_OVERLAY_GEN=1 is the off switch (every automatic first-run step has one);
// CZ_NO_PATCHED_ASSETS=1 already disables the overlay's USE in the VFS and is
// honoured here too — a run that asked for the shipped data byte-for-byte should
// not spend a first-run generating files it will then ignore.
#pragma once

#include <functional>
#include <string>

namespace OverlayGen
{
// True when generation should run at boot: the game is unpacked, no off switch is
// set, and any overlay output is missing or was written by an older generator
// (a version stamp guards against a shipped update silently serving stale banks).
bool WantedAtBoot();

// Generate everything that is missing or stale. `progress` receives a label and a
// 0..1 fraction for the first-run window; it may be null. On failure returns false
// with `err` naming the first gate that refused — the caller prints it loudly and
// boots on: the VFS then serves the shipped data, which is degraded but honest.
bool Generate(const std::function<void(const char*, float)>& progress, std::string& err);
}
