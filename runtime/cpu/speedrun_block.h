// The speedrun status block: a fixed-address, versioned struct an external timer
// (LiveSplit's auto-splitting runtime, asl, a Python script) can read to know whether
// the game is LOADING or in a CUTSCENE, and which cutscene played last.
//
// WHY IT EXISTS. A load remover locates its signals by pointer path or signature scan
// against the game executable. This port rebuilds that executable constantly — every
// part relinks 57,822 recompiled functions plus the runtime — so any offset a runner
// finds is stale within days, and a signature scan over a 149 MB binary is both slow
// and fragile. Publishing the three facts they actually need, at an address this
// runtime chooses and promises not to move, replaces that whole problem with a read.
//
// THE CONTRACT, and it is the point of the file:
//   * the block lives at CZ_SPEEDRUN_BLOCK_ADDRESS, a fixed host virtual address;
//   * `magic` and `version` identify it, and the layout below never changes meaning —
//     new fields go in `reserved`, and a field that has to change semantics gets a new
//     `version` instead;
//   * every value is little-endian host-native (x86-64 on both platforms we ship);
//   * `seq` is a seqlock: ODD while a write is in progress. Read seq, read the body,
//     read seq again; if either read is odd or they differ, retry.
//
// See docs/speedrun-block.md for the reader's side of this.
#pragma once

#include <cstdint>
#include <string>

// The address the block is mapped at. Chosen well clear of where either OS hands out
// ordinary allocations, 64 KB-aligned (Windows' allocation granularity), and inside the
// 128 TiB user half both platforms share. 0x435A is 'CZ'.
#define CZ_SPEEDRUN_BLOCK_ADDRESS 0x0000435A00000000ull

#define CZ_SPEEDRUN_BLOCK_MAGIC "CZSPDRN1"
#define CZ_SPEEDRUN_BLOCK_VERSION 1u

// 256 bytes, fixed forever. Offsets are in the comments because the reader is written
// against offsets, not against this header.
struct CzSpeedrunBlock
{
    char     magic[8];            // 0x00  "CZSPDRN1", not NUL-terminated
    uint32_t version;             // 0x08  CZ_SPEEDRUN_BLOCK_VERSION
    uint32_t size;                // 0x0C  sizeof(CzSpeedrunBlock) == 256

    uint64_t seq;                 // 0x10  seqlock; odd = a write is in progress
    uint64_t frame;               // 0x18  presented frames since launch
    uint64_t monotonicNs;         // 0x20  host steady clock at the last publish
    uint64_t guestBase;           // 0x28  host address of guest VA 0 — see the note below

    uint32_t gameState;           // 0x30  index into the engine's own state table, or
                                  //       0xFFFFFFFF if it could not be resolved
    uint32_t gameStateId;         // 0x34  the engine's interned name hash for that state
    uint8_t  isLoading;           // 0x38  gameState is FEToGame or Loading
    uint8_t  isCutscene;          // 0x39  a cinematic is running
    uint8_t  isCutsceneExclusive; // 0x3A  ... and it is an EXCLUSIVE one (a full NIS)
    uint8_t  isInGame;            // 0x3B  gameState is InGame / InGameTut1 / GameShow
    uint32_t loadCount;           // 0x3C  times isLoading went 0 -> 1
    uint32_t cutsceneCount;       // 0x40  times a cinematic started
    uint32_t reserved0;           // 0x44
    uint32_t guestFlowObject;     // 0x48  guest VA, diagnostics
    uint32_t guestCineManager;    // 0x4C  guest VA, diagnostics
    uint32_t guestCurrentCine;    // 0x50  guest VA of the running cinematic, 0 if none
    uint32_t reserved1;           // 0x54
    char     gameStateName[16];   // 0x58  "Loading", "InGame", ... NUL-terminated
    char     cutsceneName[64];    // 0x68  the cinematic that started LAST, e.g.
                                  //       "701_chuck_arrives_in_town". NUL-terminated,
                                  //       and it PERSISTS after that cinematic ends
    uint8_t  reserved2[0x58];     // 0xA8  zero; future fields come out of here
};
static_assert(sizeof(CzSpeedrunBlock) == 256, "the block's size is part of its contract");

// `guestBase` is the single most useful field for anything beyond load removal: guest
// addresses are properties of the ORIGINAL Xbox 360 executable, so they never change
// when this port is rebuilt. A reader that wants the game's own variables reads
// guestBase + <guest VA> and byte-swaps — the guest is big-endian.

// Maps the block. Safe to call once, after Memory::Init. Never fatal: if the fixed
// address is unavailable the block is allocated anywhere, the address is logged and
// written to cz_speedrun_block.txt, and the magic still makes a scan possible.
void SpeedrunBlock_Init();

// Every load and cutscene transition this session, oldest first, as text. Goes into the
// F9 bug report's system.txt so one keypress at ANY moment captures the whole session's
// history rather than whatever happened to be on screen — an operator asked to press F9
// "at or near" each cutscene, and a record that does not depend on their timing is
// strictly better than one that does. Includes the name-decode verdict per cutscene, so
// a playthrough closes the check the headless routes could not reach (open-items 0zd).
std::string SpeedrunBlock_EventLog();

// One publish per presented frame, from the PM4 swap. Cheap: a handful of guest reads
// and a 256-byte store. A no-op if Init did not run or the feature is switched off.
void SpeedrunBlock_Publish(uint8_t* base);
