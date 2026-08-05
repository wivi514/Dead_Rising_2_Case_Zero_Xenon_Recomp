// Resolution of XEX *data* imports (kernel-exported variables) plus function-IAT
// slots — the piece of the loader XenonUtils' Xex2LoadImage does not do. It only
// rewrites the 4-instruction call thunks; the type-0 address-table entries keep
// their raw ordinal encoding, which the game then happily dereferences as a
// pointer.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Copies the XEX's header block into guest memory and returns its guest address,
// so RtlImageXexHeaderField can answer from the real headers instead of a guess —
// and A1 shows that call is the very FIRST thing this title does. Publishes the
// same address as XexExecutableModuleHandle. Call before ResolveXexDataImports.
uint32_t PublishXexHeaders(const uint8_t* xexFile, size_t xexFileSize);

// Guest address of the header block published above (0 until published).
extern std::atomic<uint32_t> g_xexHeaderBase;

// Walks the XEX import descriptors (headers are plaintext in the raw file even
// though the image body here is LZX-compressed under the devkit key; the descriptor
// VAs are resolved against already-loaded guest memory) and:
//  - function imports: writes the paired call thunk's guest address into the IAT
//    slot, so code calling through the IAT lands on the recompiled import
//    dispatcher;
//  - variable imports: allocates backed storage with a sensible initial value and
//    writes its guest address into the slot.
void ResolveXexDataImports(const uint8_t* xexFile);

// Guest address of the KeTimeStampBundle storage (0 until resolved). Games read
// {interrupt time, system time, tick count} from this bundle instead of calling
// KeQuerySystemTime; the vsync pump will refresh it from phase 3 onward.
extern std::atomic<uint32_t> g_keTimeStampBundle;

// One XEX optional header, by key: its guest address, or 0 if this XEX has no such
// header. `headerBase` 0 means the block PublishXexHeaders installed.
uint32_t XexHeaderField(uint32_t headerBase, uint32_t key);

// This title's own id, out of XEX_HEADER_EXECUTION_INFO. Case Zero's is 0x58410A8D.
// The content enumerator needs it because the title FILTERS enumerated saves by it
// (sub_825D8E60), so a save carrying the wrong id is silently skipped.
uint32_t XexTitleId();
