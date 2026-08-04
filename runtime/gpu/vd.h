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

// Where the Xbox 360 maps the GPU register file into the title's address space.
// Not our choice and not a guess: Case Zero's driver kicks the ring by storing the
// write pointer straight to CP_RB_WPTR at 0x7FC80714 (the single `PPC_MM_STORE_U32`
// at that address in the whole image), and Xenia registers its MMIO handler over the
// same range. kernel/heap.cpp withholds it from the virtual arena for that reason.
constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
constexpr uint32_t kGpuRegisterSize = 0x00010000;
constexpr uint32_t kCpRbWptrAddress = 0x7FC80714;

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
