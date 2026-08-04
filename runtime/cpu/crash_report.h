// Guest-side fault reporting.
#pragma once

// Installs handlers for SIGSEGV/SIGBUS/SIGILL/SIGTRAP. Call once, before any guest
// code runs.
extern "C" void CzInstallCrashReporter();

// The same guest stack walk, on demand and without dying. Used by the stall trace in
// kernel/imports.cpp. Far too expensive for a hot path — callers gate it themselves.
extern "C" void CzDumpGuestBacktrace(const char* label);
