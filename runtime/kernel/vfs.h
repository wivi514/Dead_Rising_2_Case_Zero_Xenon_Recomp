// The guest filesystem, over the unpacked STFS package in assets/game/.
//
// WHY THIS IS IN PHASE 1 AND NOT PHASE 2
// --------------------------------------
// docs/runtime-plan.md puts the VFS in phase 2, and phase 1's gate is that our
// kernel-call sequence prefix-matches A1 out to the title screen. Those two cannot
// both be true: A1's 22nd distinct kernel call is `NtCreateFile`, and with it left
// as an honest-failure stub the title concludes it cannot read the disc and takes
// its dirty-disc path —
//
//     22  NtCreateFile               NtCreateFile
//     23  NtQueryFullAttributesFile  XamShowDirtyDiscErrorUI
//     24  NtClose                    XamLoaderLaunchTitle
//
// which is the stub behaving exactly as designed, and a hard stop for the gate. So
// the file layer moves forward into phase 1. What stays in phase 2 is the part the
// plan is really about: the `.big` archive semantics and the seek-order oracle.
//
// WHAT THE PATHS LOOK LIKE
// ------------------------
// Everything this title opens during boot is spelled `game:\...`, with backslashes:
//
//     NtCreateFile(..., "game:\layout.bin", ...)
//     NtCreateFile(..., "game:\data\preload4.big", ...)
//     NtQueryFullAttributesFile("game:\data\serial.bin", ...)
//
// `assets/game/` is the STFS package as `tools/extract_stfs.py` unpacked it, so the
// mapping is `game:` -> that directory and `\` -> `/`. Xenia registers both `GAME:`
// and `D:` as symlinks to the package before the title runs (A1's own log says so),
// so both are mounted here up front rather than waiting for the guest to ask.
//
// CASE-INSENSITIVITY IS NOT OPTIONAL
// ----------------------------------
// The 360's filesystem is case-insensitive and this title's paths are constructed
// at runtime (`anm_%s.big`), so a name assembled from a lowercase token can address
// a file the package stores capitalised. On Linux that is a silent
// file-not-found — and a missing archive presents as a missing model, not as an
// I/O error. Lookups therefore fall back to a case-insensitive scan of the
// containing directory, with the result cached.
#pragma once

#include <cstdint>
#include <string>

// `assets/game/`, deduced from the .xex path in main.cpp.
void VfsSetGameRoot(const std::string& hostPath);

// Mount `device:` (no colon, case-insensitive) at a host directory.
void VfsMountDevice(const std::string& device, const std::string& hostPath);
void VfsUnmountDevice(const std::string& device);

// Guest path -> host path, or "" if the device is not mounted. Does NOT check
// existence; see VfsResolveExisting.
std::string VfsTranslate(const std::string& guestPath);

// Guest path -> an existing host path, applying the case-insensitive fallback.
// Returns "" if nothing matches. Both answers are cached, INCLUDING the misses.
std::string VfsResolveExisting(const std::string& guestPath);

// Forget one path's cached answer. Call this after creating a file: the create's own
// existence check cached a miss, and without this the file it wrote can never be
// re-opened. Mount/unmount clear the whole cache and do not need it.
void VfsForget(const std::string& guestPath);
