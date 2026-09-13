// THE BUG-REPORT CAPTURE: F9 (one frame) or F8 (three frames over a second) writes a
// directory the XenonLive launcher's Issues tab lists — the picture, the log for the
// 60 s before the key and the 15 s after, and the machine (OS, CPU, GPU, driver, RAM,
// the game's version and settings) — so a player's report arrives with everything a
// developer needs, without the player collecting any of it.
//
// The contract is XenonLive's docs/bug-reports.md: <data dir>/captures/<name>/ with a
// capture.json naming the files, written as <name>.partial/ and renamed when whole
// (the launcher lists capture.json and a half-written directory would show as a
// broken capture). The data dir is the one libxlive resolves (XLIVE_DATA_DIR, else
// the platform's XenonLive folder), so game and launcher agree; CZ_BUG_REPORT_DIR
// overrides it. Nothing here touches the network — the launcher shows the capture,
// the player decides.
//
// The dev-side instruments on the same keys (CZ_CAPTURE_KEY's census and snapshots,
// CZ_BURST_DUMP's every-frame burst) are untouched and still fire when armed; this
// is the always-on half a shipped build carries. CZ_BUG_REPORTS=0 turns it off.
//
// The folder is bounded: CZ_BUG_REPORT_MAX_MB (default 256) and at most 40 captures;
// the oldest (by name, which is the time) are deleted first, and a .partial older
// than ten minutes is a crashed capture and is removed too.
#pragma once

#include <cstdint>

// After LogFile::Begin and before the window exists: resolves the folder, prints it.
void BugReport_Init();

// The renderer's device line, once it knows it (deviceName, the driver's own name
// and version text, the Vulkan version the device reports).
void BugReport_SetGpu(const char* device, const char* driver, uint32_t apiVersion);

// The key. `trigger` is what the player pressed ("F9" / "F8"); `frames` how many
// presented frames to keep (1, or 3 spaced ~half a second apart for F8). The report
// is written 15 s later by a worker thread; a press while one is in flight is folded
// into it (the log window is what it is) and said so in the log.
void BugReport_Request(const char* trigger, unsigned frames);

// The renderer's two calls, every presented frame: does a capture still want
// pixels (so the present readback stays armed), and here they are.
bool BugReport_WantsPixels();
void BugReport_OfferPixels(const uint8_t* rgba, uint32_t width, uint32_t height,
                           uint64_t frame);

// At exit: finish (or abandon) a report in flight so a quit right after F9 does not
// leave a .partial behind.
void BugReport_Shutdown();
