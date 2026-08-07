// The PM4 command processor — the part of the GPU the boot cannot get past.
//
// The guest's D3D driver submits work by writing PM4 packets into a ring buffer and
// kicking a write pointer; real hardware walks from its read pointer to that write
// pointer, executes the packets, and reports progress back into guest memory. Phase
// 1 recorded the ring and ignored it, and the boot stopped dead in the driver's
// free-space wait (`sub_82845160`) because nothing ever advanced the read pointer.
//
// WHAT "EXECUTING" MEANS HERE, AND WHY THAT IS ENOUGH
// ---------------------------------------------------
// Nothing is rasterised. Draw packets are parsed and counted, state-setting packets
// land in a register file, and the only packets with an effect outside this module
// are the ones that write guest memory or raise an interrupt.
//
// That is enough because what the driver is blocked on is not pixels — it is
// progress reports, and **the stream carries the answers to its own waits**. The
// words its wait loops poll are written by EVENT_WRITE* packets the guest itself put
// in the ring. So an executor that parses the stream in order and honours those
// packets releases the driver without inventing a single value, which is the only
// acceptable way to do it (gotcha 5). "Advance the read pointer and hope" would have
// been the fake, and it fails for a concrete reason: the guest is entitled to
// recycle everything behind the read pointer, so a read pointer that runs ahead of
// the parser hands it permission to overwrite packets we have not read yet.
//
// THE OPCODE SET IS KNOWN AND SMALL
// ---------------------------------
// `tools/xtr_pm4_census.py` over B1 (finding 10d): **21 distinct type-3 opcodes,
// every one named**, and the same 21 in B1b and in B2's gameplay — gameplay
// introduces none the frontend does not already use. Measured counts over B1's
// 8,283,322 type-3 packets:
//
//   0x60 SET_BIN_MASK_LO 2,353,460   0x22 DRAW_INDX    1,438,031
//   0x46 EVENT_WRITE     1,203,473   0x5A EVENT_WRITE_EXT 1,141,008
//   0x27 IM_LOAD           836,994   0x2F LOAD_ALU_CONSTANT 523,229
//   0x36 DRAW_INDX_2       205,188   0x2B IM_LOAD_IMMEDIATE 204,151
//   0x3B INVALIDATE_STATE  184,076   0x3C WAIT_REG_MEM   83,172
//   0x61 SET_BIN_MASK_HI    31,714   0x2D SET_CONSTANT   30,624
//   0x3F INDIRECT_BUFFER    28,726   0x58 EVENT_WRITE_SHD  8,026
//   0x54 INTERRUPT           4,138   0x62 SET_BIN_SELECT_LO 3,784
//   0x21 REG_RMW             1,092   0x63 SET_BIN_SELECT_HI 1,090
//   0x64 XE_SWAP             1,089   0x45 COND_WRITE        256
//   0x48 ME_INIT                 1
//
// Notably ABSENT, and worth stating because the previous port's executor turns on
// it: **EVENT_WRITE_ZPD (0x5B) does not appear at all.** Asura's Wrath's boot hung
// on the Z-pass counter writeback and needed a whole synthetic-occlusion mechanism
// to get past it. Case Zero issues no occlusion queries in the captured eras, so
// that machinery is deliberately not ported. If a `0x5B` ever shows up in our own
// stream, the unknown-opcode census below is what will say so.
//
// Everything here runs on one thread (the vblank pump in gpu/vd.cpp). The counters
// are atomic only so a future tracer can read them safely.
#pragma once

#include <cstdint>

// From VdInitializeRingBuffer. `base` is a guest virtual address, `sizeBytes` the
// ring's size (vd.cpp converts the log2 argument). Resets the read cursor — a
// re-init is a new stream, not a continuation of the old one.
void Pm4_SetRingBuffer(uint32_t base, uint32_t sizeBytes);
bool Pm4_RingInitialized();

// Execute packets from the internal cursor up to `writePtr`, a dword count taken
// from the driver's own mirror in its device struct. That count is LINEAR — it keeps
// rising past the end of the ring — while the cursor is ring-relative, so the wrap
// happens in here and nowhere else.
//
// Returns the ring-relative dword index actually reached. When the tail packet is
// only half-written (header present, body not yet) the cursor stops short of
// `writePtr` and that shorter value is what comes back; publishing THAT as the read
// pointer is the whole discipline of this module.
uint32_t Pm4_Execute(uint8_t* base, uint32_t writePtr);

// Called when the executor reaches an INTERRUPT (0x54) packet — from INSIDE the
// walk, at the packet's own position in the stream, not after it.
//
// That timing is a contract rather than a detail, and `docs/runtime-plan.md` flagged
// it before any code existed: Asura's Wrath's graphics ISR reads a callback pointer
// out of the scratch-register mirror that the stream arms immediately before the
// INTERRUPT packet and poisons immediately after, so deferring delivery to the end
// of the walk calls the poison. Whether Case Zero's ISR does the same has not been
// measured here — delivering in-position is correct either way and costs nothing,
// so it is not worth finding out the expensive way.
//
// Pass nullptr to leave interrupts undelivered and merely counted.
void Pm4_SetInterruptSink(void (*sink)());

// Where the parser has actually reached, ring-relative in dwords, and the scratch
// writeback registers — for the ring trace. The read pointer the guest polls is a
// number this module owns, so a trace that prints only what the DRIVER thinks the
// read pointer is cannot say whether we are behind or writing it somewhere else.
uint32_t Pm4_Cursor();
uint32_t Pm4_ScratchAddr();
uint32_t Pm4_ScratchUmsk();

// Indirect-buffer health, in the ring trace so it is a number rather than a hunt
// through scattered reports. `truncated` is always live and is the count of buffers
// whose walk ended early — each one drops every packet after the stop, and in this
// title the last packet of a buffer is the driver's own ring-progress fence, so a
// nonzero value here is a hang waiting to happen (finding 38). `clean`/`dirty` count
// only under CZ_PM4_IB_VERIFY.
uint64_t Pm4_IbTruncatedCount();
uint64_t Pm4_IbVerifyCleanCount();
uint64_t Pm4_IbVerifyDirtyCount();

// WAIT_REG_MEM health, and the numbers that decide whether CZ_PM4_STOP_ON_WAIT is
// safe as a default. `unmet` counts every evaluation that failed (with the brake off,
// the command processor carries on past each one). `held` counts the ticks the brake
// actually stopped a walk.
//
// `streak`/`streakmax` are how many ticks IN A ROW the ring has sat on one wait: a
// title pacing itself releases within a tick or three, a ring nothing will ever
// release grows the streak without bound. A release COUNT cannot do this job — its
// discriminator (has the stall's address changed?) reads the two arms differently,
// because phase C re-emits its hand-off block at a fixed scratch address while the
// PM4 arm's rotate through the ring. Both stall sites print sparsely, so these exist
// because the running index of a capped print is not a count (gotcha 109).
uint64_t Pm4_WaitUnmetCount();
uint64_t Pm4_RingHeldCount();
uint64_t Pm4_HoldStreak();
uint64_t Pm4_HoldStreakMax();

// The engine's fence completion word, so the command processor can recognise stores to
// it. Set from the graphics pump, which is the only place that knows the device struct.
// Only CZ_PM4_FENCE_MONOTONIC (a phase C part 7 EXPERIMENT arm) acts on it; the count
// of refused backwards stores is free to read either way.
void Pm4_SetFenceWord(uint32_t va);
uint64_t Pm4_FenceRegressionCount();

// The microcode bound by the last IM_LOAD/IM_LOAD_IMMEDIATE for a stage. `hash` is
// FNV-1a over the big-endian microcode and is the renderer's cache key; it is zero
// until a stage has been bound. See the shader-load block in pm4.cpp for why the
// identity is the content and not the address.
struct Pm4ShaderBinding
{
    uint32_t ucodeVa = 0;    // 0 for IM_LOAD_IMMEDIATE, which has no guest buffer
    uint32_t sizeDwords = 0;
    uint64_t hash = 0;
};
const Pm4ShaderBinding& Pm4_BoundShader(uint32_t stage); // 0 = vertex, 1 = pixel

// The register file. Handed to the renderer rather than copied because a draw reads a
// few dozen of 0x8000 registers and which ones depend on the draw; snapshotting the
// whole file per draw would cost more than the draw.
const uint32_t* Pm4_Registers();

// The draw seam. Set by the renderer at init; called from inside the packet walk for
// every DRAW_INDX/DRAW_INDX_2 that survives ME predication, with the bound shaders and
// the register file already current.
//
// Called from the walk rather than queued, for the same reason the interrupt sink is:
// the stream's meaning is positional. A draw's state is whatever the packets before it
// set, and a queue that defers the draw past the next SET_CONSTANT renders it with the
// following draw's state — which looks like a shader bug and is not one.
struct Pm4Draw
{
    uint32_t primType;    // VGT_DRAW_INITIATOR bits 5:0
    uint32_t indexCount;  // bits 31:16
    bool indexed;         // source select 0/1 = DMA (indexed), 2 = auto-index
    uint32_t indexVa;     // guest VA of the index buffer, 0 when not indexed
    bool index32;         // 32-bit indices rather than 16-bit
    uint32_t indexEndian; // the index buffer's own endian swizzle code
};
void Pm4_SetDrawSink(void (*sink)(uint8_t* base, const Pm4Draw&));

uint64_t Pm4_PacketCount();
uint64_t Pm4_TypeCount(uint32_t type);      // type 0..3
uint64_t Pm4_OpcodeCount(uint32_t opcode);  // type-3 opcode 0x00..0x7F
uint64_t Pm4_DrawCount();
uint64_t Pm4_FrameCount();                  // XE_SWAP packets = frames
uint64_t Pm4_InterruptCount();
const char* Pm4_OpcodeName(uint32_t opcode); // nullptr when the opcode is unknown
