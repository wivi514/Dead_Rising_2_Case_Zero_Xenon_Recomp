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
uint64_t Pm4_DrawsPredicatedOut();

// The (mask, select) pair census — the table `tools/xtr_bin_predication.py` prints for
// a Xenia capture, so the two can be compared directly. Off unless enabled.
void Pm4_BinCensusEnable();
void Pm4_BinCensusReport();
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
    // The SAME question read the other way: hardware carries a DMA index buffer's
    // swizzle in the TOP two bits of the size dword, not the low two of the address.
    // Both readings are carried so an arm can switch between them in one binary.
    uint32_t indexEndianTop;
    uint32_t indexSizeDword;
};
void Pm4_SetDrawSink(void (*sink)(uint8_t* base, const Pm4Draw&));

uint64_t Pm4_PacketCount();
// Register-write DWORDS executed, cumulative. `WriteRegister` is called once per dword
// of every SET_CONSTANT / LOAD_ALU_CONSTANT / type-0 run, and it is the leading suspect
// for the walk's own cost (`docs/perf-cpu-plan.md` §2). Differenced per second by
// `[vkprof]`, it turns the walk's milliseconds into a cost per dword.
uint64_t Pm4_RegisterWriteCount();
// The split of those dwords between the bulk run copy (part 47) and the per-dword
// fallback taken when a run's destination range touches the scratch mirror or the
// const-watch window. CZ_PM4_NO_BULK_REGS=1 forces everything down the fallback and is
// the same-binary control arm.
uint64_t Pm4_RegRunBulkDwords();
uint64_t Pm4_RegRunSlowDwords();
// Dwords the bulk path wrote that the per-dword path disagrees with, under
// CZ_PM4_VERIFY_BULK_REGS=1. Must be 0.
uint64_t Pm4_RegRunMismatches();
uint64_t Pm4_TypeCount(uint32_t type);      // type 0..3
uint64_t Pm4_OpcodeCount(uint32_t opcode);  // type-3 opcode 0x00..0x7F
// All of the counts above are kept PER WALKING THREAD as of part 48 and summed here,
// because as bus-locked atomics they were four `lock xadd`s on every packet — ~326,000
// per frame on the operator's stream, for pure instrumentation.
// CZ_PM4_ATOMIC_COUNTERS=1 restores the atomic form and is the control arm.
//
// How many of the 135 counters the two forms DISAGREE on under
// CZ_PM4_VERIFY_COUNTERS=1, which drives both from every site. Must be 0, and
// CZ_PM4_VERIFY_COUNTERS_POISON=1 must make it non-zero before that means anything.
// `threads` (optional) receives the number of threads that have ever walked a packet;
// it is 1 in this runtime and the exactness of the comparison depends on that.
uint64_t Pm4_CensusMismatches(uint64_t* threads);
// The filler-run census (part 50 item 1a). A run of consecutive type-2 no-op dwords is
// consumed by ONE `ExecutePacket` call; `Pm4_TypeCount(2) / Pm4_FillerRuns()` is the mean
// run length and therefore the factor by which the item reduces calls. It is printed
// because the item's whole value is that ratio and nothing had ever measured it — a share
// of 28.7% is consistent both with one enormous run and with 23,000 isolated dwords, and
// only the second of those makes this change worthless. CZ_PM4_NO_FILLER_RUNS=1 is the arm.
uint64_t Pm4_FillerRuns();
uint64_t Pm4_FillerRingDwords();   // ...of the type-2 dwords, those walked at ring level
uint64_t Pm4_FillerHist(uint32_t bucket);  // run length, log2 buckets: 1,2,4,8..128+
// The shader-content memo (part 52 item 1.0). `BindShader` hashed the whole microcode on
// every one of ~1,300-2,200 shader-load packets a frame, which a `perf` symbol profile
// with the instruments off put at 12.47% of the pump thread — ~71% of its samples on the
// four `imulq`s of the FNV chain. The memo answers by `memcmp` against the bytes it
// hashed last time instead. The plan's cheaper `(va, size, first, last)` PROBE key was
// built first and refuted by the verify arm below; pm4.cpp's header comment has the
// transcript and why the failure would have been silent.
//
// The hit rate is the item's own measurement and is printed by `[vkprof]`: a low one
// means the memo is thrashing rather than working, and evictions separate "the guest
// binds more shaders than the table holds" from "the guest keeps re-uploading".
// CZ_PM4_NO_SHADER_MEMO=1 is the control arm.
uint64_t Pm4_ShaderMemoHits();
uint64_t Pm4_ShaderMemoMisses();
uint64_t Pm4_ShaderMemoEvictions();
// ...of which were CAPACITY misses — every way of the set was already occupied, so the
// table shape is what cost the hit and more ways or more sets would remove it. The
// complement is a compulsory miss (a shader seen for the first time), which nothing can
// remove. Split because "N% of loads still hash" means two completely different things
// depending on which it is (gotcha 339: a share is not a shape).
uint64_t Pm4_ShaderMemoCollisions();
// Disagreements between the memo's answer and a real fold of the microcode, under
// CZ_PM4_VERIFY_SHADER_HASH=1 (which does BOTH on every load). **Must be 0**, and
// CZ_PM4_VERIFY_SHADER_POISON=1 must make it non-zero before that zero means anything.
uint64_t Pm4_ShaderMemoMismatches();
// Bumped by every write that touches the ALU constant file. See WriteRegister for why
// it is monotonic, why both write paths are covered, and what a missed bump costs.
uint64_t Pm4_AluConstVersion(uint32_t half);   // 0 = VS window, 1 = PS window
// The FETCH constant file's stamp — bumped by any register write into it. §6eb §4: the
// vertex-fetch decode is 124 ns a draw of pure re-derivation, 1.15 ms a frame at the
// operator's load, and this makes "did anything it reads change" an O(1) question.
uint64_t Pm4_FetchConstVersion();
uint64_t Pm4_DrawCount();
uint64_t Pm4_FrameCount();                  // XE_SWAP packets = frames
uint64_t Pm4_InterruptCount();
const char* Pm4_OpcodeName(uint32_t opcode); // nullptr when the opcode is unknown
