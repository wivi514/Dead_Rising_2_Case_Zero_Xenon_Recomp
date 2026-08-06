// Graphics device state (the Vd* kernel exports) and the interrupt pump.
//
// Everything the guest's D3D driver tells the kernel about the GPU lands here: where
// the PM4 ring buffer is, where the GPU should report its read pointer and its
// command-buffer completion identifier, and which guest routine to call when the GPU
// raises an interrupt. gpu/pm4.cpp consumes that state.
//
// This is its own module rather than more of kernel/imports.cpp because the Vd block
// is not really "kernel calls" — it is the handshake between the driver and the
// graphics hardware, and it belongs next to the GPU rather than next to the heap.
#pragma once

#include <cstdint>
#include <mutex>

// Where the Xbox 360 maps the GPU register file into the title's address space.
// Not our choice and not a guess: Case Zero's driver kicks the ring by storing the
// write pointer straight to CP_RB_WPTR at 0x7FC80714 (the single `PPC_MM_STORE_U32`
// at that address in the whole image), and Xenia registers its MMIO handler over the
// same range. kernel/heap.cpp withholds it from the virtual arena for that reason.
constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
constexpr uint32_t kGpuRegisterSize = 0x00010000;
constexpr uint32_t kCpRbWptrAddress = 0x7FC80714;

// The title's ENTIRE GPU MMIO surface is four registers (scan of every `lis rX,0x7FC8`
// in the image and the load/store that follows it), and only one of them is READ:
//
//   0x7FC80714  CP_RB_WPTR                       written, the ring kick
//   0x7FC83214  (engine enable, = 7)             written once, by VdInitializeEngines'
//   0x7FC83408  (            , = 0x800)          own callback sub_8284C770
//   0x7FC86110  D1GRPH_PRIMARY_SURFACE_ADDRESS   written, the scanout flip
//   0x7FC86544  the display controller's gate    READ, bit 0, once — at 82844DB0
//
// The last one is the whole of this runtime's MMIO read surface, and it decides
// whether the guest's own vblank ISR runs its swap-queue walker at all. `sub_82844D38`
// (the graphics ISR), source 0:
//
//     lis  r11,0x7FC8
//     lwz  r11,0x6544(r11)
//     clrlwi. r11,r11,31          ; bit 0
//     beq  <skip>
//     bl   sub_82841760           ; the swap-queue walker, its ONLY caller
//
// On hardware the display controller sets that bit. In our flat memory it is zero for
// the life of the process, so the walker never runs: the vblank tick [dev+0x4174]
// never advances, the 16-entry flip queue at [dev+0x418C] is never drained, and — the
// part that matters beyond presentation — the walker's "no surface to scan out" case
// is the ONLY CPU writer of the GPU/CPU rendezvous word at [[dev+0x2A94]]+4, which the
// command processor's own WAIT_REG_MEM blocks on. Fable 2 hit the identical gate at
// the identical address (its findings 48 and 57) and ended up asserting it by default;
// this is the same driver.
constexpr uint32_t kD1GrphPrimarySurfaceAddress = 0x7FC86110;
constexpr uint32_t kDisplayControllerGate = 0x7FC86544;

// Offsets into the driver's device struct — the `userData` argument of
// VdSetGraphicsInterruptCallback. Both are read out of this image's own code rather
// than inherited; the previous port's equivalents are at different offsets, so
// copying them would have been silently wrong.
//
//   +10956  the mirror of the kicked write pointer. From the ring-kick site in
//           `sub_82845698`, which is the ONLY store to CP_RB_WPTR in the image:
//               stw  r11,10956(r29)     <- mirror
//               sync
//               lis  r10,32712          ; 0x7FC8
//               stw  r11,1812(r10)      ; CP_RB_WPTR
//               eieio ; sync
//           Taking the mirror rather than the MMIO dword follows Fable 2's finding
//           48, where reading the register instead was a real regression because the
//           MMIO write is not always kick-fenced. Here both are written under one
//           fence, so the choice is free — and the pump logs both anyway so the claim
//           stays checkable rather than inherited.
//
//   +10896  a POINTER to the read-pointer write-back slot, dereferenced by the
//           driver's free-space wait in `sub_82845160`:
//               lwz  r11,10896(r31)     ; the slot's address
//               lwz  r10,10908(r31)
//               lwz  r11,0(r11)         ; how far the GPU has consumed
//           This is the loop the boot was stuck in before this module existed.
constexpr uint32_t kDeviceKickedWptr = 10956;
constexpr uint32_t kDeviceWritebackPtr = 10896;

// The swap queue, read out of `sub_82841760` (the walker) and `sub_82841878` (the
// source-1 flip callback the hand-off blocks arm — our ISR trace prints it as
// SCRATCH_REG4). Sixteen 8-byte records {surface address, due vblank tick}; a record
// whose surface is zero means "nothing to scan out, just release the GPU", and its
// completion is the store `[[dev+0x2A94]+4] = 0`.
// A POINTER to the scratch-register mirror the graphics ISR reads: word 0 is the
// six-bit per-CPU acknowledge bitmap, +0x10/+0x14 the armed callback and its argument
// (SCRATCH_REG4/5), +4 the GPU/CPU rendezvous word the swap queue clears.
constexpr uint32_t kDeviceIsrMirror = 10900; // 0x2A94

constexpr uint32_t kDeviceVblankTick = 0x4174;   // ++ per walker run
constexpr uint32_t kDeviceFlipsDone = 0x4188;    // ++ per record retired
constexpr uint32_t kDeviceSwapQueue = 0x418C;    // 16 x {surface, dueTick}
constexpr uint32_t kDeviceSwapHead = 0x420C;
constexpr uint32_t kDeviceSwapTail = 0x4210;

struct VdGraphicsState
{
    uint32_t interruptCallback; // guest routine, called as (source, userData)
    uint32_t interruptUserData; // the driver's device struct
    uint32_t ringBufferBase;    // guest virtual address of the PM4 ring
    uint32_t ringBufferSize;    // bytes
    uint32_t rptrWriteback;     // guest virtual address of the read-pointer slot
    uint32_t gpuIdentifier;     // guest virtual address of the completion id slot
};

VdGraphicsState Vd_GetState();

// The display mode this runtime reports, filled into the guest's XVIDEO_MODE.
// ONE definition, used by both VdQueryVideoMode and XGetVideoMode: A1 shows the
// guest calling XGetVideoMode twice and VdQueryVideoMode three times during the same
// display bring-up, and the driver's letterbox arithmetic straddles the two. Two
// independent copies is how they drift.
struct _XVIDEO_MODE;
void Vd_FillVideoMode(_XVIDEO_MODE* mode);

// True once the guest has registered an interrupt callback and the pump is live.
bool Vd_PumpRunning();

// Phase C: the registered graphics ISR, for the D3D walker's IN-POSITION source-1
// delivery. Two deferred designs failed before this one and both are worth
// remembering: pending a bare interrupt to the pump delivered it after the guest
// re-poisoned the scratch mirror (every movie-era interrupt skipped, boot
// deadlocked on fences the token worker never submitted); pending it WITH a mirror
// snapshot replayed at delivery still raced the guest CPU's own poison stores,
// which no host lock can intercept, and the ISR called 0x0BADF00D. The mirror
// protocol is only coherent AT the packet's own position — where pm4 delivers its
// interrupts, and where the walker now delivers too. The ISR's source-1 path
// dispatches the armed callback BEFORE it takes its spinlock, so the synchronous
// call from the engine thread does not re-enter a lock the emission site holds.
uint32_t Vd_InterruptCallbackVa();
uint32_t Vd_InterruptUserData();

// The mirror lock. The pump's in-position deliveries never race the ring walk (one
// thread), but the D3D walker writes the same mirror words from the ENGINE thread —
// and a write landing between the pump's poison check and the guest ISR's own mirror
// load hands the ISR the poison as a callback (measured: ctr=0BADF00D, lr inside the
// ISR). The walker takes this around each mirror write; the pump holds it across
// replay + delivery, ISR included.
std::recursive_mutex& Vd_MirrorMutex();
