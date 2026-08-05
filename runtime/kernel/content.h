// The content (save-data) layer: enumerators, the XAM enumerate message, and the
// mount that turns a save into a `save:\` device.
//
// Split out of imports.cpp because it is one mechanism with a lot of derivation
// behind it, and because the derivation is the valuable part for the next port —
// see content.cpp's header comment, which is where the whole chain is written down.
#pragma once

#include <cstdint>
#include <string>

#include "memory.h" // PPCFunc

// Where saves live on the host. Called from main.cpp with the directory holding
// default.xex, exactly like VfsSetGameRoot — the save root is its sibling `save/`
// unless CZ_SAVE_DIR says otherwise.
void ContentSetRootFromGameDir(const std::string& gameDir);

// The implementation bound to a dynamically-resolved xam ordinal, or nullptr if we
// have none for it. XexGetProcedureAddress uses this to decide whether to mint a
// thunk to a real function or to the generic honest-failure stub.
//
// This exists because `XamContentAggregateCreateEnumerator` is NOT in this title's
// import table: it is resolved at runtime as xam ordinal 0x279 (A1 line 111,986
// names it), so there is no `__imp__` symbol to hook and the only seam is the mint.
PPCFunc* ContentMintedExportForOrdinal(uint32_t ordinal);

// The XAM app-message seam. Returns true when this module owns the (app, message)
// pair and has written `result`; false to let the caller fall through to its own
// dispatcher. Message 0x0002000E on app 0xFE is the enumeration step — it is how
// the title's own XamEnumerate wrapper actually pulls items (see content.cpp).
bool ContentDispatchAppMessage(uint32_t app, uint32_t message, void* buffer,
                               uint32_t bufferLength, uint32_t* result);
